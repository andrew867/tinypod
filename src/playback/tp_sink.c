#include "tp_sink.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TINYPOD_HAVE_TINYALSA
#include <tinyalsa/pcm.h>
#endif

enum sink_kind {
    SINK_ALSA = 0,
    SINK_WAV
};

struct tp_sink {
    enum sink_kind kind;
    int rate;
    int channels;
#ifdef TINYPOD_HAVE_TINYALSA
    struct pcm *pcm;
#endif
    FILE *wav;
    unsigned long wav_bytes;
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

int tp_sink_available(void)
{
#ifdef TINYPOD_HAVE_TINYALSA
    return 1;
#else
    return 0;
#endif
}

/* ---------------------------------------------------------------- ALSA --- */

struct tp_sink *tp_sink_open(int rate, int channels, char *err, size_t errsz)
{
    /*
     * Capture instead of playing. Lets the whole playback path be exercised
     * where there is no sound card - a dev host, or the N31 before the codec
     * is up - and turns "I hear nothing" into a file you can look at.
     */
    const char *cap = getenv("TINYPOD_ALSA_WAV");

    if (cap && *cap)
        return tp_sink_open_wav(cap, rate, channels, err, errsz);

#ifdef TINYPOD_HAVE_TINYALSA
    {
    struct tp_sink *s;
    struct pcm_config cfg;
    unsigned int card = 0, device = 0;
    unsigned int period;
    const char *e;

    if (rate <= 0 || channels <= 0) {
        fail(err, errsz, "invalid stream format (%d Hz, %d channels)", rate, channels);
        return NULL;
    }

    s = calloc(1, sizeof(*s));
    if (!s) {
        fail(err, errsz, "out of memory");
        return NULL;
    }
    s->kind = SINK_ALSA;
    s->rate = rate;
    s->channels = channels;

    if ((e = getenv("TINYPOD_ALSA_CARD")) != NULL)
        card = (unsigned int)strtoul(e, NULL, 10);
    if ((e = getenv("TINYPOD_ALSA_DEVICE")) != NULL)
        device = (unsigned int)strtoul(e, NULL, 10);

    /*
     * 10 ms periods, four of them. Same shape n31-sine proved on the glass:
     * at 44100 that is an exact 441-frame period, and 40 ms of buffer is
     * enough to ride out a NAND read without underrunning.
     */
    period = (rate % 100u) == 0 ? (unsigned int)rate / 100u : 512u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.channels = (unsigned int)channels;
    cfg.rate = (unsigned int)rate;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.period_size = period;
    cfg.period_count = 4;
    cfg.start_threshold = period * 2;
    cfg.stop_threshold = period * 4;
    cfg.silence_threshold = 0;
    cfg.avail_min = period;

    s->pcm = pcm_open(card, device, PCM_OUT, &cfg);
    if (!s->pcm || !pcm_is_ready(s->pcm)) {
        fail(err, errsz, "cannot open ALSA card %u device %u: %s", card, device,
             s->pcm ? pcm_get_error(s->pcm) : "no such device");
        if (s->pcm)
            pcm_close(s->pcm);
        free(s);
        return NULL;
    }
    return s;
    }
#else
    (void)rate;
    (void)channels;
    fail(err, errsz, "this build has no ALSA output (rebuild with tinyalsa,\n"
                     "or set TINYPOD_ALSA_WAV to capture to a file instead)");
    return NULL;
#endif
}

/* ----------------------------------------------------------------- WAV --- */

static void put32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f);
    fputc((int)((v >> 24) & 0xff), f);
}

static void put16(FILE *f, unsigned int v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
}

struct tp_sink *tp_sink_open_wav(const char *path, int rate, int channels,
                                 char *err, size_t errsz)
{
    struct tp_sink *s;

    if (rate <= 0 || channels <= 0) {
        fail(err, errsz, "invalid stream format (%d Hz, %d channels)", rate, channels);
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        fail(err, errsz, "out of memory");
        return NULL;
    }
    s->kind = SINK_WAV;
    s->rate = rate;
    s->channels = channels;
    s->wav = fopen(path, "wb");
    if (!s->wav) {
        fail(err, errsz, "cannot write %s", path);
        free(s);
        return NULL;
    }

    /* Sizes are patched in on close. */
    fwrite("RIFF", 1, 4, s->wav);
    put32(s->wav, 0);
    fwrite("WAVEfmt ", 1, 8, s->wav);
    put32(s->wav, 16);
    put16(s->wav, 1);
    put16(s->wav, (unsigned int)channels);
    put32(s->wav, (unsigned long)rate);
    put32(s->wav, (unsigned long)(rate * channels * 2));
    put16(s->wav, (unsigned int)(channels * 2));
    put16(s->wav, 16);
    fwrite("data", 1, 4, s->wav);
    put32(s->wav, 0);
    return s;
}

/* --------------------------------------------------------------- write --- */

int tp_sink_write(struct tp_sink *s, const int16_t *pcm, int samples)
{
    if (!s || !pcm || samples <= 0)
        return 0;

    if (s->kind == SINK_WAV) {
        size_t n = fwrite(pcm, sizeof(int16_t), (size_t)samples, s->wav);
        s->wav_bytes += (unsigned long)(n * sizeof(int16_t));
        return n == (size_t)samples ? 0 : -1;
    }

#ifdef TINYPOD_HAVE_TINYALSA
    {
        unsigned int frames = (unsigned int)samples / (unsigned int)s->channels;
        if (!frames)
            return 0;
        if (pcm_writei(s->pcm, pcm, frames) < 0)
            return -1;
        return 0;
    }
#else
    return -1;
#endif
}

void tp_sink_pause(struct tp_sink *s, int paused)
{
    if (!s)
        return;
#ifdef TINYPOD_HAVE_TINYALSA
    if (s->kind == SINK_ALSA && s->pcm) {
        if (paused)
            pcm_stop(s->pcm);
        else
            pcm_prepare(s->pcm);
    }
#else
    (void)paused;
#endif
}

void tp_sink_drain(struct tp_sink *s)
{
    if (!s)
        return;
#ifdef TINYPOD_HAVE_TINYALSA
    if (s->kind == SINK_ALSA && s->pcm)
        pcm_stop(s->pcm);
#endif
}

void tp_sink_close(struct tp_sink *s)
{
    if (!s)
        return;

    if (s->kind == SINK_WAV && s->wav) {
        if (fseek(s->wav, 4, SEEK_SET) == 0)
            put32(s->wav, 36 + s->wav_bytes);
        if (fseek(s->wav, 40, SEEK_SET) == 0)
            put32(s->wav, s->wav_bytes);
        fclose(s->wav);
    }
#ifdef TINYPOD_HAVE_TINYALSA
    if (s->kind == SINK_ALSA && s->pcm)
        pcm_close(s->pcm);
#endif
    free(s);
}
