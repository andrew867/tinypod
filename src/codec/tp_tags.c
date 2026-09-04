/*
 * Reading display metadata out of an audio file.
 *
 * Three formats, in the order they are worth trusting: the ID3v2 tag in front
 * of an MP3 or an ADTS stream, the iTunes metadata atoms in an MP4, and - only
 * when nothing better turned up - the thirty-byte fixed fields of ID3v1 at the
 * end of the file.
 *
 * The whole module is written on the assumption that every length in the file
 * is a lie until it has been checked against what is actually there. These
 * files come off a FAT volume on a flash translation layer that is currently
 * handing back EIO for blocks it has lost, and they were tagged over twenty
 * years by writers that each read the specification a little differently. A
 * truncated tag, a frame that claims to be larger than the file, an encoding
 * byte nobody has ever heard of - all ordinary, none of them a reason to do
 * anything other than return what was salvaged.
 */
#include "tp_tags.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Sized off the struct rather than written out again, because a scratch buffer
 * that is one byte smaller than the field it fills is the sort of thing that
 * survives review and then truncates every title on the device.
 */
#define FIELD_MAX (sizeof ((struct tp_tags *)0)->title)

/*
 * How much of an ID3v2 tag is worth pulling into memory.
 *
 * The tag is buffered rather than seeked through because unsynchronisation
 * rewrites the frame headers as well as the frame contents: with it set there
 * is no way to find frame N+1 without having undone the rewrite over
 * everything in front of it. Buffering makes that one pass and turns every
 * subsequent bound into a comparison against a length that is known to exist.
 *
 * The cost is a ceiling, and embedded album art shares the tag and runs to
 * megabytes. Anything past this is not read. Taggers put the text frames
 * first and the picture after them, so what is given up is the unusual file
 * rather than the usual one, and a title found after reading a quarter of a
 * megabyte off a sick disk beats one found after reading four.
 */
#define ID3_TAG_MAX (256u * 1024u)

/* Enough of a text frame to hold any title in any of the four encodings; a
   UTF-16 title long enough to overrun this would not fit on the screen. */
#define ID3_TEXT_MAX 1024

/* Bounds on the work a malformed file can ask for. Neither loop can spin -
   both advance a cursor every pass - but a file that is nothing but valid
   box headers should still not be walked for a second. */
#define ID3_FRAME_MAX 256
#define MP4_BOX_MAX   512

/* Most of a data atom is never wanted; a title that needs more than this is
   not going to be displayed anyway. */
#define MP4_DATA_MAX 1024

/*
 * The atom names iTunes writes begin with 0xA9, the old Mac copyright sign.
 * They are spelled in octal because a hex escape is greedy: "\xA9ART" reads
 * the 'A' as a fifth hex digit and becomes one out-of-range character.
 */
#define ATOM_TITLE  "\251nam"
#define ATOM_ARTIST "\251ART"
#define ATOM_ALBUM  "\251alb"

enum tag_field {
    F_NONE = 0,
    F_TITLE,
    F_ARTIST,
    F_ALBUM,
    F_TRACK
};

/* --------------------------------------------------------------- bytes --- */

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t be24(const unsigned char *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const unsigned char *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

/* ID3v2 lengths carry seven bits per byte so that no length can contain a
   byte that looks like the start of an MPEG frame sync. */
static uint32_t syncsafe32(const unsigned char *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

/* ---------------------------------------------------------------- text --- */

/*
 * Append one code point, or nothing at all.
 *
 * Returning zero rather than writing what fits is the whole point: a title cut
 * in the middle of a three-byte sequence is not a shorter title, it is a
 * string the font engine will render as a replacement glyph or walk off the
 * end of. The caller stops on the first refusal.
 */
static int utf8_put(char *dst, size_t cap, size_t *o, uint32_t cp)
{
    size_t n;

    if (cp < 0x80)
        n = 1;
    else if (cp < 0x800)
        n = 2;
    else if (cp < 0x10000)
        n = 3;
    else
        n = 4;

    if (*o + n + 1 > cap)
        return 0;

    switch (n) {
    case 1:
        dst[(*o)++] = (char)cp;
        break;
    case 2:
        dst[(*o)++] = (char)(0xC0 | (cp >> 6));
        dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
        break;
    case 3:
        dst[(*o)++] = (char)(0xE0 | (cp >> 12));
        dst[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
        break;
    default:
        dst[(*o)++] = (char)(0xF0 | (cp >> 18));
        dst[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
        break;
    }
    return 1;
}

/* ISO-8859-1 is a subset of Unicode by code point, so the conversion is the
   encoder and nothing else. */
static void from_latin1(char *dst, size_t cap, const unsigned char *s, size_t n)
{
    size_t i, o = 0;

    if (!cap)
        return;
    for (i = 0; i < n && s[i]; i++) {
        if (!utf8_put(dst, cap, &o, s[i]))
            break;
    }
    dst[o] = 0;
}

/*
 * UTF-16 in either byte order.
 *
 * Surrogates are paired here rather than passed through, because the pair is
 * the character: emitting the two halves separately produces two code points
 * that are not valid on their own and that no encoder should ever have
 * written. A half with no partner is dropped for the same reason.
 */
static void from_utf16(char *dst, size_t cap, const unsigned char *s, size_t n,
                       int big)
{
    size_t i, o = 0;

    if (!cap)
        return;
    for (i = 0; i + 1 < n; i += 2) {
        uint32_t u = big ? (uint32_t)(((uint32_t)s[i] << 8) | s[i + 1])
                         : (uint32_t)(((uint32_t)s[i + 1] << 8) | s[i]);

        if (u == 0)
            break;
        if (u >= 0xD800 && u < 0xDC00) {
            uint32_t lo;

            if (i + 3 >= n)
                break;
            lo = big ? (uint32_t)(((uint32_t)s[i + 2] << 8) | s[i + 3])
                     : (uint32_t)(((uint32_t)s[i + 3] << 8) | s[i + 2]);
            if (lo < 0xDC00 || lo > 0xDFFF)
                continue;
            u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            continue;
        }
        if (!utf8_put(dst, cap, &o, u))
            break;
    }
    dst[o] = 0;
}

/*
 * UTF-8 in, UTF-8 out - decoded and re-encoded rather than copied.
 *
 * A straight copy would be faster and would also propagate whatever the tagger
 * wrote, including the overlong forms and stray continuation bytes that turn
 * up in files converted between character sets by tools that guessed. Going
 * through the code point means the output is valid by construction and the
 * truncation at the end of the buffer lands on a character boundary.
 */
static void from_utf8(char *dst, size_t cap, const unsigned char *s, size_t n)
{
    size_t i = 0, o = 0;

    if (!cap)
        return;
    /* Some writers put a byte order mark on UTF-8 as well. It is not text. */
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF)
        i = 3;

    while (i < n && s[i]) {
        uint32_t cp = s[i];
        size_t need, k;

        if (cp < 0x80) {
            need = 0;
        } else if ((cp & 0xE0) == 0xC0) {
            need = 1;
            cp &= 0x1F;
        } else if ((cp & 0xF0) == 0xE0) {
            need = 2;
            cp &= 0x0F;
        } else if ((cp & 0xF8) == 0xF0) {
            need = 3;
            cp &= 0x07;
        } else {
            i++;                       /* continuation byte with no lead */
            continue;
        }

        if (i + need >= n)
            break;                     /* cut off by the end of the frame */
        for (k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                break;
            cp = (cp << 6) | (uint32_t)(s[i + k] & 0x3F);
        }
        if (k <= need) {
            i++;                       /* malformed: drop the lead and resync */
            continue;
        }
        i += need + 1;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            continue;
        if (!utf8_put(dst, cap, &o, cp))
            break;
    }
    dst[o] = 0;
}

/* ID3v1 pads with spaces and ID3v2 writers pad with whatever they had; either
   way the trailing run is not part of the name. */
static void trim(char *s)
{
    size_t n = strlen(s), i = 0;

    while (n > 0 && (unsigned char)s[n - 1] <= ' ')
        n--;
    while (i < n && (unsigned char)s[i] == ' ')
        i++;
    if (i)
        memmove(s, s + i, n - i);
    s[n - i] = 0;
}

/*
 * The decoders above already stop on a whole code point inside a buffer this
 * size, so the truncation here is a bound that nothing reaches rather than one
 * anything relies on. It still walks back off a partial sequence, because the
 * one case that would reach it is the one where getting it wrong shows.
 */
static void copy_field(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);

    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* "5/12" is how nearly every tagger writes track five of twelve, so the
   number ends where the digits do. */
static int track_of(const char *s)
{
    int v = 0;

    while (*s == ' ')
        s++;
    if (*s < '0' || *s > '9')
        return 0;
    while (*s >= '0' && *s <= '9' && v < 100000)
        v = v * 10 + (*s++ - '0');
    return v;
}

/*
 * First writer wins.
 *
 * The readers run in order of how much they are worth trusting, so a later one
 * is only ever filling a gap an earlier one left - an ID3v1 title is a
 * thirty-byte truncation of the ID3v2 one and should never replace it.
 */
static int store(struct tp_tags *out, enum tag_field f, const char *s)
{
    if (!s[0])
        return 0;

    switch (f) {
    case F_TITLE:
        if (out->title[0])
            return 0;
        copy_field(out->title, sizeof(out->title), s);
        return 1;
    case F_ARTIST:
        if (out->artist[0])
            return 0;
        copy_field(out->artist, sizeof(out->artist), s);
        return 1;
    case F_ALBUM:
        if (out->album[0])
            return 0;
        copy_field(out->album, sizeof(out->album), s);
        return 1;
    case F_TRACK: {
        int t;

        if (out->track)
            return 0;
        t = track_of(s);
        if (t <= 0)
            return 0;
        out->track = t;
        return 1;
    }
    case F_NONE:
    default:
        return 0;
    }
}

/* --------------------------------------------------------------- ID3v2 --- */

/*
 * Undo unsynchronisation: the writer inserted a zero after every 0xFF so that
 * nothing inside the tag could be mistaken for an MPEG frame sync by a decoder
 * that skipped the header. This has to happen before anything else, because
 * the frame headers were rewritten too and their sizes do not read correctly
 * until the inserted bytes are gone.
 */
static size_t deunsync(unsigned char *p, size_t n)
{
    size_t i, o = 0;

    for (i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00)
            i++;
    }
    return o;
}

/* Real frame ids are upper case letters and digits. Anything else means the
   cursor has walked into padding or into the audio, and there is nothing
   further along worth looking at. */
static int id_ok(const unsigned char *id, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
            return 0;
    }
    return 1;
}

static enum tag_field field_of(const unsigned char *id, size_t n)
{
    if (n == 3) {
        if (!memcmp(id, "TT2", 3))
            return F_TITLE;
        if (!memcmp(id, "TP1", 3))
            return F_ARTIST;
        if (!memcmp(id, "TAL", 3))
            return F_ALBUM;
        if (!memcmp(id, "TRK", 3))
            return F_TRACK;
        return F_NONE;
    }
    if (!memcmp(id, "TIT2", 4))
        return F_TITLE;
    if (!memcmp(id, "TPE1", 4))
        return F_ARTIST;
    if (!memcmp(id, "TALB", 4))
        return F_ALBUM;
    if (!memcmp(id, "TRCK", 4))
        return F_TRACK;
    return F_NONE;
}

/*
 * A text frame's payload leads with the encoding it was written in.
 *
 * The fifth value nobody defined is treated as a frame with no encoding byte
 * at all, which is what a handful of old writers actually produced: reading
 * the first letter of the title as an encoding is the more expensive mistake.
 */
static void decode_text(char *dst, size_t cap, const unsigned char *p, size_t n)
{
    if (!cap)
        return;
    dst[0] = 0;
    if (!n)
        return;

    switch (p[0]) {
    case 0:
        from_latin1(dst, cap, p + 1, n - 1);
        break;
    case 1:
        /* The mark is required here and is sometimes missing; little endian is
           what the writers that forget it were producing. */
        if (n >= 3 && p[1] == 0xFF && p[2] == 0xFE)
            from_utf16(dst, cap, p + 3, n - 3, 0);
        else if (n >= 3 && p[1] == 0xFE && p[2] == 0xFF)
            from_utf16(dst, cap, p + 3, n - 3, 1);
        else
            from_utf16(dst, cap, p + 1, n - 1, 0);
        break;
    case 2:
        from_utf16(dst, cap, p + 1, n - 1, 1);
        break;
    case 3:
        from_utf8(dst, cap, p + 1, n - 1);
        break;
    default:
        from_latin1(dst, cap, p, n);
        break;
    }
}

/*
 * One frame's payload into one field.
 *
 * The v2.3 and v2.4 format flags sit in the same byte and disagree about what
 * each bit means, so they are read against the version rather than as one set.
 * Only the ones that move where the text starts matter: a compressed or
 * encrypted frame is skipped rather than half-decoded, and the group byte and
 * v2.4's data length indicator would otherwise be read as the encoding byte.
 */
static void take_frame(struct tp_tags *out, int ver, const unsigned char *id,
                       size_t idn, int fflags, const unsigned char *p, size_t n,
                       int tag_unsync, int *found)
{
    enum tag_field f = field_of(id, idn);
    unsigned char local[ID3_TEXT_MAX];
    char text[FIELD_MAX];

    if (f == F_NONE || n == 0)
        return;

    if (ver == 3) {
        if (fflags & 0xC0)                 /* compressed or encrypted */
            return;
        if (fflags & 0x20) {               /* group identifier */
            if (n < 2)
                return;
            p++;
            n--;
        }
    } else if (ver == 4) {
        if (fflags & 0x0C)                 /* compressed or encrypted */
            return;
        if (fflags & 0x40) {               /* group identifier */
            if (n < 2)
                return;
            p++;
            n--;
        }
        if (fflags & 0x01) {               /* data length indicator */
            if (n < 5)
                return;
            p += 4;
            n -= 4;
        }
    }

    if (n > sizeof(local))
        n = sizeof(local);
    memcpy(local, p, n);
    /* v2.4 can unsynchronise a single frame instead of the whole tag; when the
       tag flag was set the buffer has already been through this once. */
    if (ver == 4 && (fflags & 0x02) && !tag_unsync)
        n = deunsync(local, n);

    decode_text(text, sizeof(text), local, n);
    trim(text);
    if (store(out, f, text))
        *found = 1;
}

/*
 * Step over the extended header.
 *
 * v2.3 writes a length that does not count the four bytes holding it; v2.4
 * writes a syncsafe length that does. Taking one for the other lands the frame
 * loop four bytes inside a frame id, which fails the id check and throws away
 * the entire tag - so the two are kept apart rather than approximated.
 */
static int ext_header_skip(const unsigned char *buf, size_t len, int ver,
                           size_t *p)
{
    uint32_t sz;

    if (len - *p < 4)
        return 0;

    if (ver == 4) {
        if ((buf[*p] | buf[*p + 1] | buf[*p + 2] | buf[*p + 3]) & 0x80)
            return 0;
        sz = syncsafe32(buf + *p);
        if (sz < 6 || sz > len - *p)
            return 0;
        *p += sz;
        return 1;
    }

    sz = be32(buf + *p);
    if (sz > len - *p - 4)
        return 0;
    *p += 4 + sz;
    return 1;
}

static int id3v2_read(FILE *f, long fsize, struct tp_tags *out)
{
    unsigned char hdr[10];
    unsigned char *buf;
    size_t len, p, want;
    long tag_size, avail;
    int ver, flags, found = 0, frames = 0;

    if (fseek(f, 0, SEEK_SET) != 0)
        return 0;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        return 0;
    if (memcmp(hdr, "ID3", 3) != 0)
        return 0;

    ver = hdr[3];
    flags = hdr[5];
    if (ver < 2 || ver > 4)
        return 0;
    /* A length byte with its top bit set is not a syncsafe length, which means
       this is not a header we understand well enough to trust the size in. */
    if ((hdr[6] | hdr[7] | hdr[8] | hdr[9]) & 0x80)
        return 0;
    tag_size = ((long)hdr[6] << 21) | ((long)hdr[7] << 14) |
               ((long)hdr[8] << 7) | (long)hdr[9];

    avail = fsize - (long)sizeof(hdr);
    if (tag_size <= 0 || avail <= 0)
        return 0;
    /* The size is the file's own claim about itself, and a tag that says it is
       larger than the file it lives in is exactly the case this exists for. */
    if (tag_size > avail)
        tag_size = avail;

    want = (size_t)tag_size;
    if (want > ID3_TAG_MAX)
        want = ID3_TAG_MAX;
    buf = malloc(want);
    if (!buf)
        return 0;
    /* A short read is a partial tag, not a failure: the frames that did come
       off the disk are still worth parsing. */
    len = fread(buf, 1, want, f);
    if (len == 0) {
        free(buf);
        return 0;
    }

    if (flags & 0x80)
        len = deunsync(buf, len);

    p = 0;
    if (flags & 0x40) {
        /* v2.2 spells whole-tag compression in this bit, and the specification
           says a tag whose compression method is not recognised cannot be
           read - which is every one of them, since no method was ever
           defined. v2.3 and v2.4 mean an extended header here instead. */
        if (ver == 2 || !ext_header_skip(buf, len, ver, &p)) {
            free(buf);
            return 0;
        }
    }

    while (p < len && frames < ID3_FRAME_MAX) {
        size_t idn = (ver == 2) ? 3u : 4u;
        size_t hdrn = (ver == 2) ? 6u : 10u;
        size_t fsz;
        int fflags = 0;

        if (len - p < hdrn)
            break;
        /* Padding, which is how a well-formed tag ends. */
        if (buf[p] == 0 || !id_ok(buf + p, idn))
            break;

        if (ver == 2) {
            fsz = be24(buf + p + 3);
        } else if (ver == 3) {
            fsz = be32(buf + p + 4);
            fflags = buf[p + 9];
        } else {
            /* v2.4 calls for a syncsafe size and a fair number of v2.4 writers
               emitted a plain one anyway. A byte with its top bit set could
               not have come out of the syncsafe encoder, so it settles it. */
            const unsigned char *s = buf + p + 4;

            fsz = ((s[0] | s[1] | s[2] | s[3]) & 0x80) ? be32(s) : syncsafe32(s);
            fflags = buf[p + 9];
        }

        p += hdrn;
        frames++;
        /* A frame reaching past what was read means either a corrupt size or
           the end of the buffer, and nothing after it can be located either
           way. */
        if (fsz > len - p)
            break;

        take_frame(out, ver, buf + p - hdrn, idn, fflags, buf + p, fsz,
                   (flags & 0x80) != 0, &found);
        p += fsz;
    }

    free(buf);
    return found;
}

/* --------------------------------------------------------------- ID3v1 --- */

static int v1_field(struct tp_tags *out, enum tag_field f,
                    const unsigned char *p)
{
    char text[FIELD_MAX];

    from_latin1(text, sizeof(text), p, 30);
    trim(text);
    return store(out, f, text);
}

/*
 * ID3v1: three thirty-byte fields at the very end of the file, in a character
 * set the format never named. It runs last and only fills what is still empty,
 * because everything about it is worse than v2 - the fields are truncated
 * where they were written, and 8859-1 is a guess that is wrong for most of the
 * world.
 */
static int id3v1_read(FILE *f, long fsize, struct tp_tags *out)
{
    unsigned char t[128];
    int found = 0;

    if (fsize < (long)sizeof(t))
        return 0;
    if (fseek(f, fsize - (long)sizeof(t), SEEK_SET) != 0)
        return 0;
    if (fread(t, 1, sizeof(t), f) != sizeof(t))
        return 0;
    if (memcmp(t, "TAG", 3) != 0)
        return 0;

    found |= v1_field(out, F_TITLE, t + 3);
    found |= v1_field(out, F_ARTIST, t + 33);
    found |= v1_field(out, F_ALBUM, t + 63);

    /* ID3v1.1 took the last two bytes of the comment for a track number, and
       marked it by the zero that shortened the comment to make room. */
    if (t[125] == 0 && t[126] != 0 && !out->track) {
        out->track = t[126];
        found = 1;
    }
    return found;
}

/* ----------------------------------------------------------------- MP4 --- */

struct mp4_ctx {
    FILE *f;
    struct tp_tags *out;
    int found;
    int budget;
};

/*
 * One box header, leaving the stream on its payload, in the same shape
 * tp_mp4.c reads them: box_end is the absolute offset one past the box, which
 * is both where the payload stops and where the next sibling starts, so no
 * caller repeats the arithmetic.
 */
static int mp4_box(FILE *f, uint64_t limit, char type[5], uint64_t *box_end)
{
    unsigned char hdr[8];
    long pos = ftell(f);
    uint64_t start, sz;

    if (pos < 0)
        return -1;
    start = (uint64_t)pos;
    if (start + 8 > limit)
        return -1;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        return -1;

    sz = be32(hdr);
    memcpy(type, hdr + 4, 4);
    type[4] = 0;

    if (sz == 1) {
        unsigned char ext[8];

        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext))
            return -1;
        sz = be64(ext);
        if (sz < 16)
            return -1;
    } else if (sz == 0) {
        sz = limit - start;            /* runs to the end of its container */
    }
    if (sz < 8 || start + sz > limit)
        return -1;

    *box_end = start + sz;
    return 0;
}

/*
 * The value inside one metadata item.
 *
 * The four bytes in front of the value are a version and flags, and the flags
 * are where the type lives: 1 is UTF-8, 2 is UTF-16BE, 0 is whatever the atom
 * says it is. trkn says binary and means a little record - two reserved bytes,
 * the track, then the track count - which is why it cannot go through the text
 * path even though it is a number a human wrote.
 */
static void mp4_data(struct mp4_ctx *c, enum tag_field f, uint64_t end)
{
    unsigned char head[8];
    unsigned char payload[MP4_DATA_MAX];
    char text[FIELD_MAX];
    long pos = ftell(c->f);
    uint64_t avail;
    size_t n;
    uint32_t kind;

    if (pos < 0 || (uint64_t)pos + sizeof(head) > end)
        return;
    if (fread(head, 1, sizeof(head), c->f) != sizeof(head))
        return;
    kind = be24(head + 1);

    avail = end - ((uint64_t)pos + sizeof(head));
    if (avail == 0)
        return;
    n = avail > sizeof(payload) ? sizeof(payload) : (size_t)avail;
    n = fread(payload, 1, n, c->f);
    if (n == 0)
        return;

    if (f == F_TRACK) {
        int v = 0;

        if (n >= 4)
            v = (int)be16(payload + 2);
        else if (n >= 2)
            v = (int)be16(payload);
        if (v > 0 && !c->out->track) {
            c->out->track = v;
            c->found = 1;
        }
        return;
    }

    if (kind == 2)
        from_utf16(text, sizeof(text), payload, n, 1);
    else
        from_utf8(text, sizeof(text), payload, n);
    trim(text);
    if (store(c->out, f, text))
        c->found = 1;
}

static void mp4_item(struct mp4_ctx *c, enum tag_field f, uint64_t end)
{
    for (;;) {
        char type[5];
        uint64_t child_end;

        if (c->budget-- <= 0)
            return;
        if (mp4_box(c->f, end, type, &child_end) != 0)
            return;
        if (memcmp(type, "data", 4) == 0)
            mp4_data(c, f, child_end);
        if (fseek(c->f, (long)child_end, SEEK_SET) != 0)
            return;
    }
}

static void mp4_ilst(struct mp4_ctx *c, uint64_t end)
{
    for (;;) {
        char type[5];
        uint64_t item_end;
        enum tag_field f = F_NONE;

        if (c->budget-- <= 0)
            return;
        if (mp4_box(c->f, end, type, &item_end) != 0)
            return;

        if (!memcmp(type, ATOM_TITLE, 4))
            f = F_TITLE;
        else if (!memcmp(type, ATOM_ARTIST, 4))
            f = F_ARTIST;
        else if (!memcmp(type, ATOM_ALBUM, 4))
            f = F_ALBUM;
        else if (!memcmp(type, "trkn", 4))
            f = F_TRACK;

        if (f != F_NONE)
            mp4_item(c, f, item_end);
        if (fseek(c->f, (long)item_end, SEEK_SET) != 0)
            return;
    }
}

/*
 * meta is declared a full box - a version and flags in front of its children -
 * and a good number of muxers write it without them. Which it is can be
 * decided by looking rather than guessed: the four bytes at the start of a
 * child box are its size, and the four after that are its type, which is
 * always printable. Guessing wrong costs the whole ilst and every tag in it.
 */
static int meta_enter(FILE *f, uint64_t end)
{
    unsigned char b[8];
    long pos = ftell(f);
    int i, printable = 1;

    if (pos < 0 || (uint64_t)pos + sizeof(b) > end)
        return -1;
    if (fread(b, 1, sizeof(b), f) != sizeof(b))
        return -1;
    for (i = 4; i < 8; i++) {
        if (b[i] < 0x20 || b[i] > 0x7E) {
            printable = 0;
            break;
        }
    }
    return fseek(f, pos + (printable ? 0 : 4), SEEK_SET);
}

/*
 * Only the path to the metadata is walked - moov, udta, meta, ilst - and not
 * the sample tables underneath, which are large, are none of this module's
 * business, and are what tp_mp4.c is for.
 */
static void mp4_walk(struct mp4_ctx *c, uint64_t end, int depth)
{
    if (depth > 8)
        return;

    for (;;) {
        char type[5];
        uint64_t box_end;

        if (c->budget-- <= 0)
            return;
        if (mp4_box(c->f, end, type, &box_end) != 0)
            return;

        if (!memcmp(type, "moov", 4) || !memcmp(type, "udta", 4))
            mp4_walk(c, box_end, depth + 1);
        else if (!memcmp(type, "meta", 4) && meta_enter(c->f, box_end) == 0)
            mp4_walk(c, box_end, depth + 1);
        else if (!memcmp(type, "ilst", 4))
            mp4_ilst(c, box_end);

        if (fseek(c->f, (long)box_end, SEEK_SET) != 0)
            return;
    }
}

static int mp4_read(FILE *f, long fsize, struct tp_tags *out)
{
    struct mp4_ctx c;

    if (fseek(f, 0, SEEK_SET) != 0)
        return 0;
    memset(&c, 0, sizeof(c));
    c.f = f;
    c.out = out;
    c.budget = MP4_BOX_MAX;
    mp4_walk(&c, (uint64_t)fsize, 0);
    return c.found;
}

/* ---------------------------------------------------------------- read --- */

int tp_tags_read(const char *path, struct tp_tags *out)
{
    unsigned char head[12];
    FILE *f;
    long fsize;
    int found;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!path || !path[0])
        return 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0 ||
        fread(head, 1, sizeof(head), f) != sizeof(head)) {
        fclose(f);
        return 0;
    }

    /*
     * The container decides which reader runs, on the one signature that is
     * reliable: ftyp is the first box of every MP4 this device will see. An
     * MP3 whose first frame happens to read as a box header would otherwise
     * cost a pass through the atom walker for nothing.
     */
    if (memcmp(head + 4, "ftyp", 4) == 0) {
        found = mp4_read(f, fsize, out);
    } else {
        found = id3v2_read(f, fsize, out);
        if (!out->title[0] || !out->artist[0] || !out->album[0] || !out->track)
            found |= id3v1_read(f, fsize, out);
    }

    fclose(f);
    return found ? 1 : 0;
}
