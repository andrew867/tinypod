#include "tp_mp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ASC 64

struct stsc_run {
    uint32_t first_chunk;      /* 1-based */
    uint32_t samples_per_chunk;
};

struct tp_mp4 {
    FILE *f;

    char codec[5];
    int is_aac;
    int channels;
    int sample_rate;
    unsigned char asc[MAX_ASC];
    size_t asc_len;

    uint32_t timescale;
    uint64_t duration;         /* in timescale units */

    uint32_t sample_count;
    uint32_t uniform_size;     /* 0 => per-sample sizes in ->sizes */
    uint32_t *sizes;

    uint32_t chunk_count;
    uint64_t *chunk_off;

    uint32_t stsc_count;
    struct stsc_run *stsc;

    /* frame cursor */
    uint32_t cur_sample;
    uint32_t cur_chunk;        /* 1-based */
    uint32_t in_chunk;
    uint32_t stsc_idx;
    uint64_t cur_off;
};

/* ------------------------------------------------------------------ io --- */

static int rd(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n ? 0 : -1;
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint64_t be64(const unsigned char *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static int rd_u32(FILE *f, uint32_t *v)
{
    unsigned char b[4];
    if (rd(f, b, 4) != 0)
        return -1;
    *v = be32(b);
    return 0;
}

/*
 * Read one box header, leaving the stream on its payload. box_end is the
 * absolute offset one past the box, which is both where the payload stops and
 * where the next sibling starts - so callers never redo the size arithmetic.
 */
static int box_header(FILE *f, uint64_t limit, char type[5], uint64_t *box_end)
{
    unsigned char hdr[8];
    long pos = ftell(f);
    uint64_t start, sz;

    if (pos < 0)
        return -1;
    start = (uint64_t)pos;
    if (start + 8 > limit)
        return -1;
    if (rd(f, hdr, 8) != 0)
        return -1;

    sz = be32(hdr);
    memcpy(type, hdr + 4, 4);
    type[4] = 0;

    if (sz == 1) {
        unsigned char ext[8];
        if (rd(f, ext, 8) != 0)
            return -1;
        sz = be64(ext);
        if (sz < 16)
            return -1;
    } else if (sz == 0) {
        sz = limit - start;        /* box runs to the end of its container */
    }
    if (sz < 8 || start + sz > limit)
        return -1;

    *box_end = start + sz;
    return 0;
}

/* ------------------------------------------------------------ esds/asc --- */

/* MPEG-4 descriptor length: 7 bits per byte, top bit continues. */
static int desc_len(FILE *f, uint32_t *len)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        unsigned char b;
        if (rd(f, &b, 1) != 0)
            return -1;
        v = (v << 7) | (uint32_t)(b & 0x7f);
        if (!(b & 0x80))
            break;
    }
    *len = v;
    return 0;
}

/*
 * esds nests ES_Descriptor(0x03) > DecoderConfigDescriptor(0x04) >
 * DecSpecificInfo(0x05); that last one is the AudioSpecificConfig the decoder
 * is configured from. Walk the tags - the offsets are not fixed.
 */
static int parse_esds(struct tp_mp4 *m, uint64_t end)
{
    if (fseek(m->f, 4, SEEK_CUR) != 0)   /* full box version+flags */
        return -1;

    while ((uint64_t)ftell(m->f) < end) {
        unsigned char tag;
        uint32_t len;

        if (rd(m->f, &tag, 1) != 0)
            return -1;
        if (desc_len(m->f, &len) != 0)
            return -1;

        if (tag == 0x03) {
            unsigned char flags;
            if (fseek(m->f, 2, SEEK_CUR) != 0)      /* ES_ID */
                return -1;
            if (rd(m->f, &flags, 1) != 0)
                return -1;
            if ((flags & 0x80) && fseek(m->f, 2, SEEK_CUR) != 0)
                return -1;                           /* dependsOn_ES_ID */
            if (flags & 0x40) {                      /* URL */
                unsigned char ul;
                if (rd(m->f, &ul, 1) != 0 || fseek(m->f, ul, SEEK_CUR) != 0)
                    return -1;
            }
            if ((flags & 0x20) && fseek(m->f, 2, SEEK_CUR) != 0)
                return -1;                           /* OCR_ES_Id */
            continue;                                /* descend */
        }
        if (tag == 0x04) {
            /* objectTypeIndication + streamType/bufferSize/bitrates */
            if (fseek(m->f, 13, SEEK_CUR) != 0)
                return -1;
            continue;                                /* descend */
        }
        if (tag == 0x05) {
            size_t n = len > MAX_ASC ? MAX_ASC : len;
            if (rd(m->f, m->asc, n) != 0)
                return -1;
            m->asc_len = n;
            return 0;
        }
        if (fseek(m->f, (long)len, SEEK_CUR) != 0)
            return -1;
    }
    return 0;
}

/* stsd > mp4a: a fixed AudioSampleEntry, then child boxes, esds among them. */
static int parse_stsd(struct tp_mp4 *m, uint64_t end)
{
    unsigned char entry[28];
    char type[5];
    uint64_t entry_end;
    uint32_t count;

    if (fseek(m->f, 4, SEEK_CUR) != 0)   /* version+flags */
        return -1;
    if (rd_u32(m->f, &count) != 0 || count == 0)
        return -1;

    if (box_header(m->f, end, type, &entry_end) != 0)
        return -1;
    memcpy(m->codec, type, 5);
    m->is_aac = (strcmp(type, "mp4a") == 0);

    /* SampleEntry(8) + AudioSampleEntry(20): channels at 16, rate 16.16 at 24 */
    if (rd(m->f, entry, sizeof(entry)) != 0)
        return -1;
    m->channels = be16(entry + 16);
    m->sample_rate = be16(entry + 24);

    while ((uint64_t)ftell(m->f) < entry_end) {
        char t2[5];
        uint64_t child_end;

        if (box_header(m->f, entry_end, t2, &child_end) != 0)
            break;
        if (strcmp(t2, "esds") == 0)
            (void)parse_esds(m, child_end);
        if (fseek(m->f, (long)child_end, SEEK_SET) != 0)
            break;
    }
    return 0;
}

/* ---------------------------------------------------------- stbl tables --- */

static int parse_stsz(struct tp_mp4 *m)
{
    uint32_t i;

    if (fseek(m->f, 4, SEEK_CUR) != 0)
        return -1;
    if (rd_u32(m->f, &m->uniform_size) != 0)
        return -1;
    if (rd_u32(m->f, &m->sample_count) != 0)
        return -1;
    if (m->uniform_size != 0 || m->sample_count == 0)
        return 0;

    m->sizes = calloc(m->sample_count, sizeof(*m->sizes));
    if (!m->sizes)
        return -1;
    for (i = 0; i < m->sample_count; i++) {
        if (rd_u32(m->f, &m->sizes[i]) != 0)
            return -1;
    }
    return 0;
}

static int parse_stsc(struct tp_mp4 *m)
{
    uint32_t i;

    if (fseek(m->f, 4, SEEK_CUR) != 0)
        return -1;
    if (rd_u32(m->f, &m->stsc_count) != 0 || m->stsc_count == 0)
        return -1;
    m->stsc = calloc(m->stsc_count, sizeof(*m->stsc));
    if (!m->stsc)
        return -1;
    for (i = 0; i < m->stsc_count; i++) {
        uint32_t ignored;
        if (rd_u32(m->f, &m->stsc[i].first_chunk) != 0 ||
            rd_u32(m->f, &m->stsc[i].samples_per_chunk) != 0 ||
            rd_u32(m->f, &ignored) != 0)
            return -1;
    }
    return 0;
}

static int parse_stco(struct tp_mp4 *m, int wide)
{
    uint32_t i;

    if (fseek(m->f, 4, SEEK_CUR) != 0)
        return -1;
    if (rd_u32(m->f, &m->chunk_count) != 0 || m->chunk_count == 0)
        return -1;
    m->chunk_off = calloc(m->chunk_count, sizeof(*m->chunk_off));
    if (!m->chunk_off)
        return -1;
    for (i = 0; i < m->chunk_count; i++) {
        if (wide) {
            unsigned char b[8];
            if (rd(m->f, b, 8) != 0)
                return -1;
            m->chunk_off[i] = be64(b);
        } else {
            uint32_t v;
            if (rd_u32(m->f, &v) != 0)
                return -1;
            m->chunk_off[i] = v;
        }
    }
    return 0;
}

static int parse_mdhd(struct tp_mp4 *m)
{
    unsigned char ver, flags[3];

    if (rd(m->f, &ver, 1) != 0 || rd(m->f, flags, 3) != 0)
        return -1;
    if (ver == 1) {
        unsigned char b[8];
        if (rd(m->f, b, 8) != 0 || rd(m->f, b, 8) != 0)
            return -1;                                /* creation, modification */
        if (rd_u32(m->f, &m->timescale) != 0)
            return -1;
        if (rd(m->f, b, 8) != 0)
            return -1;
        m->duration = be64(b);
    } else {
        uint32_t v;
        if (rd_u32(m->f, &v) != 0 || rd_u32(m->f, &v) != 0)
            return -1;
        if (rd_u32(m->f, &m->timescale) != 0)
            return -1;
        if (rd_u32(m->f, &v) != 0)
            return -1;
        m->duration = v;
    }
    return 0;
}

/* ------------------------------------------------------------- walking --- */

struct walk_state {
    int is_sound;      /* the trak being walked said hdlr == 'soun' */
    int have_track;    /* a sound track has been committed */
};

static int walk(struct tp_mp4 *m, uint64_t end, struct walk_state *st, int depth);

static void drop_tables(struct tp_mp4 *m)
{
    free(m->sizes);
    free(m->stsc);
    free(m->chunk_off);
    m->sizes = NULL;
    m->stsc = NULL;
    m->chunk_off = NULL;
    m->sample_count = 0;
    m->chunk_count = 0;
    m->stsc_count = 0;
    m->uniform_size = 0;
    m->is_aac = 0;
    m->asc_len = 0;
}

static int walk_child(struct tp_mp4 *m, const char *type, uint64_t box_end,
                      struct walk_state *st, int depth)
{
    if (strcmp(type, "moov") == 0 || strcmp(type, "mdia") == 0 ||
        strcmp(type, "minf") == 0 || strcmp(type, "stbl") == 0)
        return walk(m, box_end, st, depth + 1);

    if (strcmp(type, "trak") == 0) {
        int rc;

        if (st->have_track)
            return 0;
        st->is_sound = 0;
        rc = walk(m, box_end, st, depth + 1);
        /* A video or text trak leaves tables behind: discard and try the next */
        if (st->is_sound)
            st->have_track = 1;
        else
            drop_tables(m);
        return rc;
    }

    if (strcmp(type, "hdlr") == 0) {
        unsigned char b[12];   /* version+flags, pre_defined, handler_type */
        if (rd(m->f, b, sizeof(b)) != 0)
            return -1;
        if (memcmp(b + 8, "soun", 4) == 0)
            st->is_sound = 1;
        return 0;
    }
    if (strcmp(type, "mdhd") == 0)
        return parse_mdhd(m);
    if (strcmp(type, "stsd") == 0)
        return parse_stsd(m, box_end);
    if (strcmp(type, "stsz") == 0)
        return parse_stsz(m);
    if (strcmp(type, "stsc") == 0)
        return parse_stsc(m);
    if (strcmp(type, "stco") == 0)
        return parse_stco(m, 0);
    if (strcmp(type, "co64") == 0)
        return parse_stco(m, 1);

    return 0;
}

static int walk(struct tp_mp4 *m, uint64_t end, struct walk_state *st, int depth)
{
    if (depth > 8)
        return 0;

    for (;;) {
        char type[5];
        uint64_t box_end;

        if (box_header(m->f, end, type, &box_end) != 0)
            return 0;
        /* A child that fails is not fatal: seek past it and keep going. */
        (void)walk_child(m, type, box_end, st, depth);
        if (fseek(m->f, (long)box_end, SEEK_SET) != 0)
            return 0;
    }
}

/* ---------------------------------------------------------------- open --- */

static void cursor_reset(struct tp_mp4 *m)
{
    m->cur_sample = 0;
    m->cur_chunk = 1;
    m->in_chunk = 0;
    m->stsc_idx = 0;
    m->cur_off = m->chunk_count ? m->chunk_off[0] : 0;
}

static void fail(char *err, size_t errsz, const char *msg)
{
    if (err && errsz)
        snprintf(err, errsz, "%s", msg);
}

struct tp_mp4 *tp_mp4_open(const char *path, char *err, size_t errsz)
{
    struct tp_mp4 *m;
    struct walk_state st;
    long fsize;

    m = calloc(1, sizeof(*m));
    if (!m) {
        fail(err, errsz, "out of memory");
        return NULL;
    }
    m->f = fopen(path, "rb");
    if (!m->f) {
        fail(err, errsz, "cannot open file");
        free(m);
        return NULL;
    }
    if (fseek(m->f, 0, SEEK_END) != 0 || (fsize = ftell(m->f)) <= 0 ||
        fseek(m->f, 0, SEEK_SET) != 0) {
        fail(err, errsz, "cannot size file");
        tp_mp4_close(m);
        return NULL;
    }

    memset(&st, 0, sizeof(st));
    walk(m, (uint64_t)fsize, &st, 0);

    if (!st.have_track) {
        fail(err, errsz, "no audio track in this MP4");
        tp_mp4_close(m);
        return NULL;
    }
    if (!m->sample_count || !m->chunk_count || !m->stsc_count) {
        fail(err, errsz, "incomplete MP4 sample tables");
        tp_mp4_close(m);
        return NULL;
    }
    cursor_reset(m);
    return m;
}

void tp_mp4_close(struct tp_mp4 *m)
{
    if (!m)
        return;
    if (m->f)
        fclose(m->f);
    free(m->sizes);
    free(m->stsc);
    free(m->chunk_off);
    free(m);
}

const unsigned char *tp_mp4_asc(const struct tp_mp4 *m, size_t *len)
{
    if (!m || !m->asc_len)
        return NULL;
    if (len)
        *len = m->asc_len;
    return m->asc;
}

int tp_mp4_is_aac(const struct tp_mp4 *m)
{
    return m ? m->is_aac : 0;
}

const char *tp_mp4_codec(const struct tp_mp4 *m)
{
    return m ? m->codec : "";
}

int tp_mp4_channels(const struct tp_mp4 *m)
{
    return m ? m->channels : 0;
}

int tp_mp4_sample_rate(const struct tp_mp4 *m)
{
    return m ? m->sample_rate : 0;
}

uint32_t tp_mp4_frame_count(const struct tp_mp4 *m)
{
    return m ? m->sample_count : 0;
}

uint64_t tp_mp4_duration_ms(const struct tp_mp4 *m)
{
    if (!m || !m->timescale)
        return 0;
    return (m->duration * 1000ull) / m->timescale;
}

void tp_mp4_rewind(struct tp_mp4 *m)
{
    if (m)
        cursor_reset(m);
}

/* Samples per chunk for the stsc run covering this chunk. */
static uint32_t spc_for_chunk(const struct tp_mp4 *m, uint32_t chunk)
{
    uint32_t i = m->stsc_idx;

    while (i + 1 < m->stsc_count && m->stsc[i + 1].first_chunk <= chunk)
        i++;
    return m->stsc[i].samples_per_chunk;
}

int tp_mp4_next_frame(struct tp_mp4 *m, unsigned char *buf, size_t bufsz)
{
    uint32_t size, spc;

    if (!m || m->cur_sample >= m->sample_count)
        return 0;

    size = m->uniform_size ? m->uniform_size : m->sizes[m->cur_sample];
    if (size == 0 || (size_t)size > bufsz)
        return -1;

    if (fseek(m->f, (long)m->cur_off, SEEK_SET) != 0)
        return -1;
    if (rd(m->f, buf, size) != 0)
        return -1;

    m->cur_off += size;
    m->cur_sample++;
    m->in_chunk++;

    while (m->stsc_idx + 1 < m->stsc_count &&
           m->stsc[m->stsc_idx + 1].first_chunk <= m->cur_chunk)
        m->stsc_idx++;
    spc = spc_for_chunk(m, m->cur_chunk);

    if (m->in_chunk >= spc) {
        m->cur_chunk++;
        m->in_chunk = 0;
        if (m->cur_chunk <= m->chunk_count)
            m->cur_off = m->chunk_off[m->cur_chunk - 1];
    }
    return (int)size;
}
