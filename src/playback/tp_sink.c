#include "tp_sink.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TINYPOD_HAVE_TINYALSA
#include <tinyalsa/pcm.h>

/*
 * tinyalsa 2.0.0 exports pcm_state but forgets to declare it: the PCM_STATE_*
 * values it returns are all documented in pcm.h, and the function itself is
 * not. Declaring it here is a matching prototype, so a version that does
 * declare it will agree rather than conflict.
 *
 * It is the only way to see an underrun from out here. tinyalsa recovers from
 * one inside pcm_writei - it re-prepares and retries - and returns success,
 * so a stream restarting several times a second is indistinguishable from one
 * playing cleanly unless you look at the state on the way in.
 */
int pcm_state(struct pcm *pcm);
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

    /* What the device actually gave us, and how badly it is coping. */
    unsigned int  period_frames;
    unsigned int  periods;
    unsigned long restarts;
    int           started;

    /*
     * What the caller feeds, when that is not what the device takes.
     *
     * tinyalsa talks to hw:0,0 and converts nothing. alsa-lib's "default"
     * is plughw, which resamples and remixes in userspace - which is why
     * mpg123 and ffplay play a 44.1 kHz track on a device that only accepts
     * 48 kHz, and we did not. Rather than refuse the track, open at a rate
     * the device will take and convert on the way in.
     */
    int      src_rate;
    int      src_channels;
    int16_t  last[2];        /* previous input frame, for interpolation */
    int      have_last;
    uint32_t pos_q16;        /* where we are between last and the next frame */
    int16_t *conv;
    size_t   conv_frames;    /* capacity, in device frames */
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
    s->src_rate = rate;
    s->src_channels = channels;
    s->fd = -1;

    if ((e = getenv("TINYPOD_ALSA_CARD")) != NULL)
        card = (unsigned int)strtoul(e, NULL, 10);
    if ((e = getenv("TINYPOD_ALSA_DEVICE")) != NULL)
        device = (unsigned int)strtoul(e, NULL, 10);

    /*
     * How much audio to keep queued, and why it is no longer 40 ms.
     *
     * This was 10 ms periods, four of them - the shape n31-sine proved on the
     * glass. n31-sine generates a tone into the buffer and does nothing else.
     * A player decodes a frame and reads the next one off a vfat mount on
     * NAND, and either of those can take longer than 20 ms on this machine
     * whenever it feels like it.
     *
     * When the buffer runs dry ALSA stops the stream, and tinyalsa quietly
     * re-prepares and starts it again on the next write - so nothing here
     * ever saw an error. What it costs is on the other side: this codec takes
     * a 60 ms rate-change settle inside its play-start, so every underrun is
     * a fragment of audio followed by 60 ms of silence, over and over. That
     * is what "codec_play_start" repeating in the log means.
     *
     * A ladder rather than one shape, because the driver is still being
     * brought up and we do not know what it will accept. Biggest first; the
     * last rung is the old shape, so this cannot open fewer devices than it
     * used to. TINYPOD_ALSA_PERIOD_MS and TINYPOD_ALSA_PERIODS pin one for
     * trying things on the device without a rebuild.
     */
    static const struct { unsigned int ms, count; } SHAPES[] = {
        { 40, 8 },   /* 320 ms */
        { 20, 8 },   /* 160 ms */
        { 20, 4 },   /*  80 ms */
        { 10, 4 },   /*  40 ms - what this used to be, and what stuttered */
    };
    unsigned int want_ms = 0, want_count = 0;
    unsigned int i, n = sizeof SHAPES / sizeof SHAPES[0];
    char tried[160] = "";
    char cando[128] = "";

    /*
     * Ask the device what it takes before asking it for something else.
     *
     * tinyalsa opens hw:0,0 and converts nothing, so a rate the codec does
     * not do is simply a failed open - and the message was "would not open",
     * which is true and useless. alsa-lib's "default" goes through plughw and
     * converts, which is the whole reason mpg123 and ffplay play tracks we
     * refuse. So: find the rate range, move into it if we have to, and say
     * what the device offered when we still cannot.
     */
    {
        struct pcm_params *pp = pcm_params_get(card, device, PCM_OUT);

        if (pp) {
            unsigned int rmin = pcm_params_get_min(pp, PCM_PARAM_RATE);
            unsigned int rmax = pcm_params_get_max(pp, PCM_PARAM_RATE);
            unsigned int cmin = pcm_params_get_min(pp, PCM_PARAM_CHANNELS);
            unsigned int cmax = pcm_params_get_max(pp, PCM_PARAM_CHANNELS);

            snprintf(cando, sizeof cando,
                     "device does %u-%u Hz, %u-%u channels",
                     rmin, rmax, cmin, cmax);

            if (rmin && rmax && ((unsigned)rate < rmin || (unsigned)rate > rmax)) {
                /* 48000 first: it is what this codec is usually clocked at,
                   and resampling 44100 to it is the common case. */
                unsigned int pick = 48000;
                if (pick < rmin || pick > rmax)
                    pick = (unsigned)rate < rmin ? rmin : rmax;
                s->rate = (int)pick;
            }
            if (cmin && (unsigned)channels < cmin)
                s->channels = (int)cmin;
            if (cmax && (unsigned)s->channels > cmax)
                s->channels = (int)cmax;

            pcm_params_free(pp);
        }
    }

    rate = s->rate;
    channels = s->channels;

    if ((e = getenv("TINYPOD_ALSA_PERIOD_MS")) != NULL)
        want_ms = (unsigned int)strtoul(e, NULL, 10);
    if ((e = getenv("TINYPOD_ALSA_PERIODS")) != NULL)
        want_count = (unsigned int)strtoul(e, NULL, 10);

    for (i = 0; i < n; i++) {
        unsigned int ms = want_ms ? want_ms : SHAPES[i].ms;
        unsigned int count = want_count ? want_count : SHAPES[i].count;
        size_t used = strlen(tried);

        /* A whole number of frames, or a power of two if the rate will not
           divide evenly - 44100 gives 882 at 20 ms, 22050 gives 441. */
        period = ((unsigned int)rate * ms) / 1000u;
        if (period == 0 || ((unsigned int)rate * ms) % 1000u)
            period = 1024u;

        memset(&cfg, 0, sizeof(cfg));
        cfg.channels = (unsigned int)channels;
        cfg.rate = (unsigned int)rate;
        cfg.format = PCM_FORMAT_S16_LE;
        cfg.period_size = period;
        cfg.period_count = count;
        /*
         * Start with the buffer full rather than half full. Half a buffer is
         * half the time to the first underrun, and the decoder is at its
         * slowest at the beginning of a track - the file is being opened and
         * the first frame parsed while the device is already playing.
         */
        cfg.start_threshold = period * count;
        cfg.stop_threshold = period * count;
        cfg.silence_threshold = 0;
        cfg.avail_min = period;

        s->pcm = pcm_open(card, device, PCM_OUT, &cfg);
        if (s->pcm && pcm_is_ready(s->pcm)) {
            s->period_frames = period;
            s->periods = count;
            return s;
        }
        snprintf(tried + used, sizeof(tried) - used, "%s%ux%ums",
                 used ? ", " : "", count, ms);
        if (s->pcm) {
            pcm_close(s->pcm);
            s->pcm = NULL;
        }
        if (want_ms || want_count)
            break;                      /* pinned by hand: one attempt only */
    }

    fail(err, errsz, "ALSA card %u device %u would not open at %d Hz %d ch.\n%s\ntried %s",
         card, device, rate, channels,
         cando[0] ? cando : "device did not say what it supports",
         tried[0] ? tried : "nothing");
    free(s);
    return NULL;
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

/* ---------------------------------------------------- rate and channels --- */
/*
 * Linear interpolation, and no apology for it.
 *
 * This exists so a track whose rate the codec will not take plays at all,
 * which it did not before: tinyalsa converts nothing, so 44.1 kHz into a
 * 48 kHz-only device was a failed open and an error on screen. Linear
 * resampling has audible imaging above about 15 kHz on a 44.1 -> 48 ratio.
 * It is still the right trade against silence, and against carrying a
 * polyphase filter and its tables in a binary that lives in 55 MiB of RAM.
 *
 * The wide build links swresample already and could do better; that is worth
 * doing once there is a device to hear the difference on.
 *
 * State carries across calls - the last input frame and the fractional
 * position - because the decoder hands us arbitrary block sizes and a
 * resampler that restarts every block clicks at every boundary.
 */
static int convert(struct tp_sink *s, const int16_t *in, int in_frames)
{
    uint32_t step;
    size_t need;
    int out = 0;
    int i;

    if (in_frames <= 0)
        return 0;

    step = (uint32_t)(((uint64_t)s->src_rate << 16) / (uint32_t)s->rate);
    if (!step)
        step = 1;

    /* Worst case out frames, plus a frame of slack for the fraction. */
    need = (size_t)((uint64_t)in_frames * (uint32_t)s->rate /
                    (uint32_t)s->src_rate) + 2;
    if (need > s->conv_frames) {
        int16_t *nb = realloc(s->conv, need * (size_t)s->channels *
                                       sizeof(int16_t));
        if (!nb)
            return -1;
        s->conv = nb;
        s->conv_frames = need;
    }

    if (!s->have_last) {
        s->last[0] = in[0];
        s->last[1] = s->src_channels > 1 ? in[1] : in[0];
        s->have_last = 1;
    }

    for (i = 0; i < in_frames; i++) {
        int16_t cur0 = in[(size_t)i * s->src_channels];
        int16_t cur1 = s->src_channels > 1
                           ? in[(size_t)i * s->src_channels + 1] : cur0;

        while (s->pos_q16 < 65536u && (size_t)out < s->conv_frames) {
            uint32_t f = s->pos_q16;
            int32_t l = s->last[0] + (((int32_t)cur0 - s->last[0]) * (int32_t)f >> 16);
            int32_t r = s->last[1] + (((int32_t)cur1 - s->last[1]) * (int32_t)f >> 16);

            s->conv[(size_t)out * s->channels] = (int16_t)l;
            if (s->channels > 1)
                s->conv[(size_t)out * s->channels + 1] = (int16_t)r;
            out++;
            s->pos_q16 += step;
        }
        s->pos_q16 -= 65536u;
        s->last[0] = cur0;
        s->last[1] = cur1;
    }
    return out;
}

/*
 * The same code the sink runs, reachable from a test.
 *
 * This conversion only happens on a device whose codec refuses the track's
 * rate, which is not something a host build can arrange - so without a hook
 * the resampler would ship having never been executed. It builds its own
 * sink rather than taking a second copy of the arithmetic.
 */
int tp_sink_convert_test(int src_rate, int dst_rate, int src_ch, int dst_ch,
                         const int16_t *in, int in_frames,
                         int16_t *out, int out_cap_frames)
{
    struct tp_sink s;
    int got;

    if (!in || !out || src_rate <= 0 || dst_rate <= 0)
        return -1;

    memset(&s, 0, sizeof s);
    s.kind = SINK_ALSA;
    s.rate = dst_rate;
    s.channels = dst_ch;
    s.src_rate = src_rate;
    s.src_channels = src_ch;

    got = convert(&s, in, in_frames);
    if (got > out_cap_frames)
        got = out_cap_frames;
    if (got > 0)
        memcpy(out, s.conv, (size_t)got * (size_t)dst_ch * sizeof(int16_t));
    free(s.conv);
    return got;
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
        unsigned int frames;
        const char *p;

        if (s->src_rate != s->rate || s->src_channels != s->channels) {
            int got = convert(s, pcm, samples / s->src_channels);
            if (got < 0)
                return -1;
            if (got == 0)
                return 0;
            pcm = s->conv;
            samples = got * s->channels;
        }

        frames = (unsigned int)samples / (unsigned int)s->channels;
        p = (const char *)pcm;

        if (!frames)
            return 0;

        /*
         * Count restarts before writing. tinyalsa recovers from an underrun
         * by itself - it re-prepares and retries inside pcm_writei - so a
         * stream that is stopping and starting several times a second looks
         * from here exactly like one that is playing perfectly. The only
         * sign is the state on the way in: once we have started, anything
         * but RUNNING means the last buffer ran dry.
         */
        if (s->started) {
            int st = pcm_state(s->pcm);
            if (st != PCM_STATE_RUNNING && st != PCM_STATE_DRAINING)
                s->restarts++;
        }

        /*
         * Write until it is all gone. pcm_writei returns the number of frames
         * taken, which may be fewer than asked for; treating any non-negative
         * answer as "all of it" dropped the remainder silently.
         */
        while (frames > 0) {
            int got = pcm_writei(s->pcm, p, frames);
            if (got < 0)
                return -1;
            if (got == 0)
                continue;
            s->started = 1;
            p += (size_t)got * (size_t)s->channels * sizeof(int16_t);
            frames -= (unsigned int)got;
        }
        return 0;
    }
#endif
    return -1;
}

unsigned long tp_sink_restarts(const struct tp_sink *s)
{
    return s ? s->restarts : 0;
}

int tp_sink_describe(const struct tp_sink *s, char *out, size_t cap)
{
    if (!out || !cap)
        return -1;
    if (!s) {
        snprintf(out, cap, "none");
        return 0;
    }
    if (s->kind == SINK_WAV) {
        snprintf(out, cap, "wav capture");
        return 0;
    }
    if (s->kind == SINK_OSS) {
        snprintf(out, cap, "oss");
        return 0;
    }
    if (s->periods && s->rate > 0) {
        unsigned long ms = (unsigned long)s->period_frames * s->periods *
                           1000ul / (unsigned long)s->rate;
        if (s->src_rate != s->rate)
            snprintf(out, cap, "alsa %lu ms %d>%d", ms, s->src_rate, s->rate);
        else
            snprintf(out, cap, "alsa %lu ms", ms);
    } else {
        snprintf(out, cap, "alsa");
    }
    return 0;
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
    if (s->kind == SINK_ALSA && s->pcm) {
        /*
         * Play out what is queued, rather than throwing it away.
         *
         * This called pcm_stop() alone, which drops the buffer - so the last
         * of every track was cut off, and with the buffer now a third of a
         * second that would be plainly audible. tinyalsa has no drain, so
         * push a buffer of silence through instead: the write blocks until
         * the hardware has taken it, by which time the real audio in front
         * of it has been played.
         */
        if (s->started && s->period_frames) {
            size_t bytes = (size_t)s->period_frames *
                           (size_t)s->channels * sizeof(int16_t);
            int16_t *quiet = calloc(1, bytes);

            if (quiet) {
                unsigned int i;
                for (i = 0; i < s->periods; i++)
                    if (pcm_writei(s->pcm, quiet, s->period_frames) < 0)
                        break;
                free(quiet);
            }
        }
        pcm_stop(s->pcm);
    }
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
    free(s->conv);
#endif
#ifdef TINYPOD_HAVE_OSS
    if (s->kind == SINK_OSS && s->fd >= 0)
        close(s->fd);
#endif
    free(s);
}
