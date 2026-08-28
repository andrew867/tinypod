#include "tp_sink.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TINYPOD_HAVE_TINYALSA
#include <tinyalsa/pcm.h>
#endif

/*
 * OSS is the fallback. The N31 kernel exposes both: tinyalsa talks to
 * /dev/snd, which is the path n31-sine proved, and the OSS emulation puts the
 * same stream on /dev/dsp. Which one is present depends on how the audio
 * modules came up, so try one and fall back to the other rather than making
 * the user care.
 */
#if defined(__has_include)
#if __has_include(<sys/soundcard.h>)
#define TINYPOD_HAVE_OSS 1
#endif
#endif

#ifdef TINYPOD_HAVE_OSS
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>
#endif

enum sink_kind {
    SINK_ALSA = 0,
    SINK_OSS,
    SINK_WAV
};

struct tp_sink {
    enum sink_kind kind;
    int rate;
    int channels;
#ifdef TINYPOD_HAVE_TINYALSA
    struct pcm *pcm;
#endif
    int fd;              /* OSS */
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
#if defined(TINYPOD_HAVE_TINYALSA) || defined(TINYPOD_HAVE_OSS)
    return 1;
#else
    return 0;
#endif
}

/* ---------------------------------------------------------------- ALSA --- */

static struct tp_sink *alsa_open(int rate, int channels, char *err, size_t errsz)
{
#ifdef TINYPOD_HAVE_TINYALSA
    struct tp_sink *s;
    struct pcm_config cfg;
    unsigned int card = 0, device = 0;
    unsigned int period;
    const char *e;

    s = calloc(1, sizeof(*s));
    if (!s) {
        fail(err, errsz, "out of memory");
        return NULL;
    }
    s->kind = SINK_ALSA;
    s->rate = rate;
    s->channels = channels;
    s->fd = -1;

    if ((e = getenv("TINYPOD_ALSA_CARD")) != NULL)
        card = (unsigned int)strtoul(e, NULL, 10);
    if ((e = getenv("TINYPOD_ALSA_DEVICE")) != NULL)
        device = (unsigned int)strtoul(e, NULL, 10);

    /*
     * 10 ms periods, four of them. Same shape n31-sine proved on the glass:
     * at 44100 that is an exact 441-frame period, and 40 ms of buffer is
     * enough to ride out a NAND read without underrunning.
     */
    period = (rate % 100) == 0 ? (unsigned int)rate / 100u : 512u;

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
        fail(err, errsz, "ALSA card %u device %u: %s", card, device,
             s->pcm ? pcm_get_error(s->pcm) : "no such device");
        if (s->pcm)
            pcm_close(s->pcm);
        free(s);
        return NULL;
    }
    return s;
#else
    (void)rate;
    (void)channels;
    fail(err, errsz, "built without tinyalsa");
    return NULL;
#endif
}

/* ----------------------------------------------------------------- OSS --- */

static struct tp_sink *oss_open(int rate, int channels, char *err, size_t errsz)
{
#ifdef TINYPOD_HAVE_OSS
    struct tp_sink *s;
    const char *dev = getenv("TINYPOD_OSS_DEVICE");
    int fmt = AFMT_S16_LE;
    int ch = channels;
    int sp = rate;

    if (!dev || !*dev)
        dev = "/dev/dsp";

    s = calloc(1, sizeof(*s));
    if (!s) {
        fail(err, errsz, "out of memory");
        return NULL;
    }
    s->kind = SINK_OSS;
    s->rate = rate;
    s->channels = channels;

    s->fd = open(dev, O_WRONLY);
    if (s->fd < 0) {
        fail(err, errsz, "%s: cannot open", dev);
        free(s);
        return NULL;
    }

    /*
     * Set format, channels and rate explicitly and check what came back. An
     * unconfigured /dev/dsp defaults to 8 kHz 8-bit mono and accepts a 44.1
     * kHz stereo stream anyway - draining it about sixty times too slowly,
     * which sounds exactly like a broken driver. Better to refuse.
     */
    if (ioctl(s->fd, SNDCTL_DSP_SETFMT, &fmt) < 0 || fmt != AFMT_S16_LE) {
        fail(err, errsz, "%s: will not accept 16-bit little-endian samples", dev);
        close(s->fd);
        free(s);
        return NULL;
    }
    if (ioctl(s->fd, SNDCTL_DSP_CHANNELS, &ch) < 0 || ch != channels) {
        fail(err, errsz, "%s: wanted %d channels, got %d", dev, channels, ch);
        close(s->fd);
        free(s);
        return NULL;
    }
    if (ioctl(s->fd, SNDCTL_DSP_SPEED, &sp) < 0) {
        fail(err, errsz, "%s: cannot set %d Hz", dev, rate);
        close(s->fd);
        free(s);
        return NULL;
    }
    /* Drivers are allowed to land near the asked-for rate, but not far off. */
    if (sp < rate - rate / 50 || sp > rate + rate / 50) {
        fail(err, errsz, "%s: wanted %d Hz, got %d Hz", dev, rate, sp);
        close(s->fd);
        free(s);
        return NULL;
    }
    s->rate = sp;
    return s;
#else
    (void)rate;
    (void)channels;
    fail(err, errsz, "built without OSS support");
    return NULL;
#endif
}

/* -------------------------------------------------------------- chooser --- */

struct tp_sink *tp_sink_open(int rate, int channels, char *err, size_t errsz)
{
    /*
     * Capture instead of playing. Lets the whole playback path be exercised
     * where there is no sound card - a dev host, or the N31 before the codec
     * is up - and turns "I hear nothing" into a file you can look at.
     */
    const char *cap = getenv("TINYPOD_ALSA_WAV");
    const char *want = getenv("TINYPOD_AUDIO");
    char alsa_err[192] = "";
    char oss_err[192] = "";
    struct tp_sink *s;

    if (rate <= 0 || channels <= 0) {
        fail(err, errsz, "invalid stream format (%d Hz, %d channels)", rate, channels);
        return NULL;
    }
    if (cap && *cap)
        return tp_sink_open_wav(cap, rate, channels, err, errsz);

    if (want && strcmp(want, "oss") == 0) {
        s = oss_open(rate, channels, err, errsz);
        return s;
    }
    if (want && strcmp(want, "alsa") == 0) {
        s = alsa_open(rate, channels, err, errsz);
        return s;
    }

    s = alsa_open(rate, channels, alsa_err, sizeof(alsa_err));
    if (s)
        return s;
    s = oss_open(rate, channels, oss_err, sizeof(oss_err));
    if (s)
        return s;

    fail(err, errsz,
         "no audio output available.\n"
         "  ALSA: %s\n"
         "  OSS:  %s\n"
         "Load the audio modules (load-mods periph), or set TINYPOD_ALSA_WAV\n"
         "to capture to a file instead.",
         alsa_err[0] ? alsa_err : "not built in",
         oss_err[0] ? oss_err : "not built in");
    return NULL;
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
    s->fd = -1;
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

#ifdef TINYPOD_HAVE_OSS
    if (s->kind == SINK_OSS) {
        const char *p = (const char *)pcm;
        size_t left = (size_t)samples * sizeof(int16_t);

        while (left > 0) {
            ssize_t n = write(s->fd, p, left);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            p += n;
            left -= (size_t)n;
        }
        return 0;
    }
#endif

#ifdef TINYPOD_HAVE_TINYALSA
    if (s->kind == SINK_ALSA) {
        unsigned int frames = (unsigned int)samples / (unsigned int)s->channels;
        if (!frames)
            return 0;
        if (pcm_writei(s->pcm, pcm, frames) < 0)
            return -1;
        return 0;
    }
#endif
    return -1;
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
        return;
    }
#endif
#ifdef TINYPOD_HAVE_OSS
    /*
     * OSS has no stop-and-keep-the-stream. Tell the driver to play out what
     * it already has rather than sit waiting for a full fragment, and let the
     * next write start it again.
     */
    if (s->kind == SINK_OSS && s->fd >= 0 && paused)
        ioctl(s->fd, SNDCTL_DSP_POST, 0);
#endif
    (void)paused;
}

void tp_sink_drain(struct tp_sink *s)
{
    if (!s)
        return;
#ifdef TINYPOD_HAVE_TINYALSA
    if (s->kind == SINK_ALSA && s->pcm)
        pcm_stop(s->pcm);
#endif
#ifdef TINYPOD_HAVE_OSS
    if (s->kind == SINK_OSS && s->fd >= 0)
        ioctl(s->fd, SNDCTL_DSP_SYNC, 0);
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
#ifdef TINYPOD_HAVE_OSS
    if (s->kind == SINK_OSS && s->fd >= 0)
        close(s->fd);
#endif
    free(s);
}
