#include "tp_decode.h"
#include "tp_mp4.h"

#include "aacdec.h"
#include "mp3dec.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Biggest AAC frame we will hand the decoder in one go. */
#define AAC_FRAME_MAX 2048
/* MP3 read-ahead. A 320 kbit/s frame is ~1440 bytes, so this holds several. */
#define MP3_BUF 8192

enum dec_kind {
    DEC_AAC_MP4 = 0,
    DEC_AAC_ADTS,
    DEC_MP3,
    DEC_WAV
};

struct tp_dec {
    enum dec_kind kind;
    int rate;
    int channels;
    uint64_t duration_ms;
    char codec[16];

    /* first block, decoded during open so rate/channels are known up front */
    int16_t primed[TP_DEC_MAX_BLOCK];
    int primed_len;
    int primed_used;

    FILE *f;

    /* AAC */
    HAACDecoder aac;
    struct tp_mp4 *mp4;
    unsigned char frame[AAC_FRAME_MAX];

    /* MP3 / ADTS streaming buffer */
    HMP3Decoder mp3;
    unsigned char buf[MP3_BUF];
    int buf_len;
    int buf_pos;
    int eof;

    /* WAV */
    long pcm_left;

    /* MP3: for the duration estimate */
    long data_start;
    long file_size;
};

static void fail(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;

    if (!err || !errsz)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- misc --- */

static const int asc_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,  7350,  0,     0,     0
};

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

static int ext_is(const char *path, const char *ext)
{
    const char *e = ext_of(path);
    size_t i;

    for (i = 0; e[i] && ext[i]; i++) {
        char a = e[i], b = ext[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return e[i] == 0 && ext[i] == 0;
}

/* -------------------------------------------------------------- buffer --- */

/* Slide the unread tail down and top the buffer up from the file. */
static int refill(struct tp_dec *d)
{
    size_t got;

    if (d->buf_pos > 0) {
        memmove(d->buf, d->buf + d->buf_pos, (size_t)d->buf_len);
        d->buf_pos = 0;
    }
    if (d->eof || d->buf_len >= (int)sizeof(d->buf))
        return d->buf_len;

    got = fread(d->buf + d->buf_len, 1, sizeof(d->buf) - (size_t)d->buf_len, d->f);
    if (got == 0)
        d->eof = 1;
    d->buf_len += (int)got;
    return d->buf_len;
}

/* ID3v2 sits in front of the first MP3 frame; its size is syncsafe (7 bits). */
static void skip_id3(FILE *f)
{
    unsigned char h[10];
    long size;

    if (fread(h, 1, sizeof(h), f) != sizeof(h)) {
        rewind(f);
        return;
    }
    if (memcmp(h, "ID3", 3) != 0) {
        rewind(f);
        return;
    }
    size = ((long)(h[6] & 0x7f) << 21) | ((long)(h[7] & 0x7f) << 14) |
           ((long)(h[8] & 0x7f) << 7) | (long)(h[9] & 0x7f);
    if (fseek(f, 10 + size, SEEK_SET) != 0)
        rewind(f);
}

/* ----------------------------------------------------------------- AAC --- */

/*
 * MP4 carries raw AAC frames with no ADTS header, so the decoder cannot infer
 * anything: hand it the channel count and core rate from the
 * AudioSpecificConfig (5 bits object type, 4 bits rate index, 4 bits channels).
 */
static int aac_configure_from_asc(struct tp_dec *d, char *err, size_t errsz)
{
    const unsigned char *asc;
    size_t asc_len = 0;
    AACFrameInfo fi;
    int obj_type, rate_idx, chans;

    asc = tp_mp4_asc(d->mp4, &asc_len);
    memset(&fi, 0, sizeof(fi));

    if (asc && asc_len >= 2) {
        obj_type = (asc[0] >> 3) & 0x1f;
        rate_idx = (int)(((asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01));
        chans = (asc[1] >> 3) & 0x0f;
        fi.sampRateCore = asc_rates[rate_idx & 0x0f];
        fi.nChans = chans;
        /* 2 = LC. 5 (SBR) and 29 (PS) both decode as LC plus SBR on top. */
        fi.profile = AAC_PROFILE_LC;
        (void)obj_type;
    }
    if (!fi.nChans)
        fi.nChans = tp_mp4_channels(d->mp4);
    if (!fi.sampRateCore)
        fi.sampRateCore = tp_mp4_sample_rate(d->mp4);

    if (!fi.nChans || !fi.sampRateCore) {
        fail(err, errsz, "AAC track has no usable stream configuration");
        return -1;
    }
    if (fi.nChans > AAC_MAX_NCHANS) {
        fail(err, errsz, "AAC track has %d channels; this build decodes up to %d",
             fi.nChans, AAC_MAX_NCHANS);
        return -1;
    }
    if (AACSetRawBlockParams(d->aac, 0, &fi) != 0) {
        fail(err, errsz, "AAC decoder rejected the stream configuration");
        return -1;
    }
    d->rate = fi.sampRateCore;
    d->channels = fi.nChans;
    return 0;
}

/* One MP4 frame in, one decoded block out. */
static int aac_mp4_block(struct tp_dec *d, int16_t *out)
{
    unsigned char *ptr;
    int len, left, rc;
    AACFrameInfo fi;

    for (;;) {
        len = tp_mp4_next_frame(d->mp4, d->frame, sizeof(d->frame));
        if (len == 0)
            return 0;
        if (len < 0)
            return -1;

        ptr = d->frame;
        left = len;
        rc = AACDecode(d->aac, &ptr, &left, out);
        if (rc == ERR_AAC_NONE)
            break;
        /*
         * One bad frame is not fatal. iTunes files often end on a partial
         * frame, and a damaged one mid-track should cost a block, not the
         * track: the cursor has already advanced, so just take the next.
         */
    }

    AACGetLastFrameInfo(d->aac, &fi);
    if (fi.sampRateOut)
        d->rate = fi.sampRateOut;
    if (fi.nChans)
        d->channels = fi.nChans;
    return fi.outputSamps;
}

/* Raw .aac: ADTS headers in the stream, so the decoder self-configures. */
static int aac_adts_block(struct tp_dec *d, int16_t *out)
{
    AACFrameInfo fi;

    for (;;) {
        unsigned char *ptr;
        int left, rc, sync;

        if (d->buf_len < 2048 && !d->eof)
            refill(d);
        if (d->buf_len <= 0)
            return 0;

        sync = AACFindSyncWord(d->buf + d->buf_pos, d->buf_len);
        if (sync < 0) {
            d->buf_pos = 0;
            d->buf_len = 0;
            if (d->eof)
                return 0;
            continue;
        }
        d->buf_pos += sync;
        d->buf_len -= sync;

        ptr = d->buf + d->buf_pos;
        left = d->buf_len;
        rc = AACDecode(d->aac, &ptr, &left, out);
        if (rc == ERR_AAC_INDATA_UNDERFLOW) {
            if (d->eof)
                return 0;
            refill(d);
            continue;
        }
        d->buf_pos += d->buf_len - left;
        d->buf_len = left;
        if (rc != ERR_AAC_NONE) {
            /* Skip the bad byte and resync. */
            if (d->buf_len > 0) {
                d->buf_pos++;
                d->buf_len--;
            }
            continue;
        }
        break;
    }

    AACGetLastFrameInfo(d->aac, &fi);
    if (fi.sampRateOut)
        d->rate = fi.sampRateOut;
    if (fi.nChans)
        d->channels = fi.nChans;
    return fi.outputSamps;
}

/* ----------------------------------------------------------------- MP3 --- */

static int mp3_block(struct tp_dec *d, int16_t *out)
{
    MP3FrameInfo fi;

    for (;;) {
        unsigned char *ptr;
        int left, rc, sync;

        if (d->buf_len < 2048 && !d->eof)
            refill(d);
        if (d->buf_len <= 0)
            return 0;

        sync = MP3FindSyncWord(d->buf + d->buf_pos, d->buf_len);
        if (sync < 0) {
            /* Keep the last two bytes: a sync word can straddle a refill. */
            d->buf_pos += d->buf_len > 2 ? d->buf_len - 2 : 0;
            d->buf_len = d->buf_len > 2 ? 2 : d->buf_len;
            if (d->eof)
                return 0;
            refill(d);
            continue;
        }
        d->buf_pos += sync;
        d->buf_len -= sync;

        ptr = d->buf + d->buf_pos;
        left = d->buf_len;
        rc = MP3Decode(d->mp3, &ptr, &left, out, 0);
        if (rc == ERR_MP3_INDATA_UNDERFLOW) {
            if (d->eof)
                return 0;
            refill(d);
            continue;
        }
        d->buf_pos += d->buf_len - left;
        d->buf_len = left;
        if (rc != ERR_MP3_NONE) {
            if (d->buf_len > 0) {
                d->buf_pos++;
                d->buf_len--;
            }
            continue;
        }
        break;
    }

    MP3GetLastFrameInfo(d->mp3, &fi);
    if (fi.samprate)
        d->rate = fi.samprate;
    if (fi.nChans)
        d->channels = fi.nChans;
    return fi.outputSamps;
}

/* ----------------------------------------------------------------- WAV --- */

static int wav_parse(struct tp_dec *d, char *err, size_t errsz)
{
    unsigned char hdr[12];
    int have_fmt = 0;

    if (fread(hdr, 1, sizeof(hdr), d->f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fail(err, errsz, "not a RIFF/WAVE file");
        return -1;
    }

    for (;;) {
        unsigned char ch[8];
        unsigned long size;

        if (fread(ch, 1, sizeof(ch), d->f) != sizeof(ch)) {
            fail(err, errsz, "WAV has no data chunk");
            return -1;
        }
        size = (unsigned long)ch[4] | ((unsigned long)ch[5] << 8) |
               ((unsigned long)ch[6] << 16) | ((unsigned long)ch[7] << 24);

        if (memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), d->f) != sizeof(fmt)) {
                fail(err, errsz, "WAV fmt chunk is truncated");
                return -1;
            }
            if ((fmt[0] | (fmt[1] << 8)) != 1) {
                fail(err, errsz, "WAV is compressed; only 16-bit PCM is supported");
                return -1;
            }
            d->channels = fmt[2] | (fmt[3] << 8);
            d->rate = (int)((unsigned)fmt[4] | ((unsigned)fmt[5] << 8) |
                            ((unsigned)fmt[6] << 16) | ((unsigned)fmt[7] << 24));
            if ((fmt[14] | (fmt[15] << 8)) != 16) {
                fail(err, errsz, "WAV is not 16-bit; only 16-bit PCM is supported");
                return -1;
            }
            have_fmt = 1;
            if (size > sizeof(fmt) &&
                fseek(d->f, (long)(size - sizeof(fmt)), SEEK_CUR) != 0)
                return -1;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) {
                fail(err, errsz, "WAV data chunk precedes its fmt chunk");
                return -1;
            }
            d->pcm_left = (long)size;
            break;
        } else if (fseek(d->f, (long)(size + (size & 1)), SEEK_CUR) != 0) {
            fail(err, errsz, "WAV chunk walk failed");
            return -1;
        }
    }

    if (!d->channels || !d->rate) {
        fail(err, errsz, "WAV header has no usable format");
        return -1;
    }
    d->duration_ms = (uint64_t)d->pcm_left * 1000ull /
                     ((uint64_t)d->rate * (uint64_t)d->channels * 2ull);
    return 0;
}

static int wav_block(struct tp_dec *d, int16_t *out)
{
    size_t want, got;

    if (d->pcm_left <= 0)
        return 0;
    want = TP_DEC_MAX_BLOCK * sizeof(int16_t);
    if ((long)want > d->pcm_left)
        want = (size_t)d->pcm_left;

    got = fread(out, 1, want, d->f);
    d->pcm_left -= (long)got;
    return (int)(got / sizeof(int16_t));
}

/* ---------------------------------------------------------------- open --- */

static int decode_block(struct tp_dec *d, int16_t *out)
{
    switch (d->kind) {
    case DEC_AAC_MP4:  return aac_mp4_block(d, out);
    case DEC_AAC_ADTS: return aac_adts_block(d, out);
    case DEC_MP3:      return mp3_block(d, out);
    case DEC_WAV:      return wav_block(d, out);
    }
    return -1;
}

struct tp_dec *tp_dec_open(const char *path, char *err, size_t errsz)
{
    struct tp_dec *d = calloc(1, sizeof(*d));

    if (!d) {
        fail(err, errsz, "out of memory");
        return NULL;
    }

    if (ext_is(path, "m4a") || ext_is(path, "m4b") || ext_is(path, "mp4") ||
        ext_is(path, "m4p")) {
        char mp4err[128] = "";

        d->kind = DEC_AAC_MP4;
        d->mp4 = tp_mp4_open(path, mp4err, sizeof(mp4err));
        if (!d->mp4) {
            fail(err, errsz, "%s", mp4err[0] ? mp4err : "cannot read MP4 container");
            tp_dec_close(d);
            return NULL;
        }
        if (!tp_mp4_is_aac(d->mp4)) {
            const char *c = tp_mp4_codec(d->mp4);
            if (strcmp(c, "alac") == 0)
                fail(err, errsz, "Apple Lossless is not supported by this build");
            else
                fail(err, errsz, "unsupported MP4 codec '%s'", c);
            tp_dec_close(d);
            return NULL;
        }
        d->aac = AACInitDecoder();
        if (!d->aac) {
            fail(err, errsz, "could not start the AAC decoder");
            tp_dec_close(d);
            return NULL;
        }
        if (aac_configure_from_asc(d, err, errsz) != 0) {
            tp_dec_close(d);
            return NULL;
        }
        d->duration_ms = tp_mp4_duration_ms(d->mp4);
        snprintf(d->codec, sizeof(d->codec), "AAC");
    } else if (ext_is(path, "aac") || ext_is(path, "adts")) {
        d->kind = DEC_AAC_ADTS;
        d->f = fopen(path, "rb");
        if (!d->f) {
            fail(err, errsz, "cannot open file");
            tp_dec_close(d);
            return NULL;
        }
        d->aac = AACInitDecoder();
        if (!d->aac) {
            fail(err, errsz, "could not start the AAC decoder");
            tp_dec_close(d);
            return NULL;
        }
        snprintf(d->codec, sizeof(d->codec), "AAC");
    } else if (ext_is(path, "wav")) {
        d->kind = DEC_WAV;
        d->f = fopen(path, "rb");
        if (!d->f) {
            fail(err, errsz, "cannot open file");
            tp_dec_close(d);
            return NULL;
        }
        if (wav_parse(d, err, errsz) != 0) {
            tp_dec_close(d);
            return NULL;
        }
        snprintf(d->codec, sizeof(d->codec), "WAV");
    } else {
        d->kind = DEC_MP3;
        d->f = fopen(path, "rb");
        if (!d->f) {
            fail(err, errsz, "cannot open file");
            tp_dec_close(d);
            return NULL;
        }
        skip_id3(d->f);
        d->data_start = ftell(d->f);
        if (fseek(d->f, 0, SEEK_END) == 0) {
            d->file_size = ftell(d->f);
            if (fseek(d->f, d->data_start, SEEK_SET) != 0)
                d->file_size = 0;
        }
        d->mp3 = MP3InitDecoder();
        if (!d->mp3) {
            fail(err, errsz, "could not start the MP3 decoder");
            tp_dec_close(d);
            return NULL;
        }
        snprintf(d->codec, sizeof(d->codec), "MP3");
    }

    /*
     * Decode the first block now. That settles rate and channel count for
     * good - with HE-AAC the output rate is twice the container's - and it
     * turns "this file cannot be decoded" into an error at open, before the
     * UI has claimed to be playing anything.
     */
    d->primed_len = decode_block(d, d->primed);
    if (d->primed_len < 0) {
        fail(err, errsz, "no decodable audio in this file");
        tp_dec_close(d);
        return NULL;
    }
    if (d->primed_len == 0 && d->kind != DEC_WAV) {
        fail(err, errsz, "no decodable audio in this file");
        tp_dec_close(d);
        return NULL;
    }
    if (!d->rate || !d->channels) {
        fail(err, errsz, "could not determine sample rate or channels");
        tp_dec_close(d);
        return NULL;
    }

    /*
     * MP3 carries no duration: estimate it from the bitrate of the first
     * frame and the size of the audio data. Right for CBR, approximate for
     * VBR - enough for a progress bar, and better than showing nothing.
     */
    if (d->kind == DEC_MP3 && d->file_size > d->data_start) {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d->mp3, &fi);
        if (fi.bitrate > 0)
            d->duration_ms = (uint64_t)(d->file_size - d->data_start) * 8000ull /
                             (uint64_t)fi.bitrate;
    }
    return d;
}

void tp_dec_close(struct tp_dec *d)
{
    if (!d)
        return;
    if (d->aac)
        AACFreeDecoder(d->aac);
    if (d->mp3)
        MP3FreeDecoder(d->mp3);
    if (d->mp4)
        tp_mp4_close(d->mp4);
    if (d->f)
        fclose(d->f);
    free(d);
}

int tp_dec_rate(const struct tp_dec *d)
{
    return d ? d->rate : 0;
}

int tp_dec_channels(const struct tp_dec *d)
{
    return d ? d->channels : 0;
}

uint64_t tp_dec_duration_ms(const struct tp_dec *d)
{
    return d ? d->duration_ms : 0;
}

const char *tp_dec_codec_name(const struct tp_dec *d)
{
    return d ? d->codec : "";
}

int tp_dec_read(struct tp_dec *d, int16_t *out)
{
    if (!d || !out)
        return -1;
    if (!d->primed_used) {
        d->primed_used = 1;
        if (d->primed_len > 0) {
            memcpy(out, d->primed, (size_t)d->primed_len * sizeof(int16_t));
            return d->primed_len;
        }
    }
    return decode_block(d, out);
}
