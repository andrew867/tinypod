#include "tp_sink.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>

#ifdef TP_WITH_SOXR
#include <soxr.h>
#endif
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

#ifdef TINYPOD_HAVE_ALSALIB
/*
 * alsa-lib's pcm.h declares a zero-length array, which -Wpedantic rejects.
 * It is a third-party header and not ours to correct, so the warning is
 * suppressed for the include and restored immediately after: every line of
 * this file is still held to the same standard as the rest of the tree.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <alsa/asoundlib.h>
#pragma GCC diagnostic pop

/*
 * alsa-lib, wearing tinyalsa's interface.
 *
 * Why at all
 * ----------
 *
 * On this device tinyalsa's view of the stream is wrong. The status page it
 * mmaps never updates, so the hardware pointer it reads is always zero, the
 * state it reports never changes, and it re-prepares the stream before every
 * write. From out here that looks like a stream stuck in PREPARED for ever:
 * one write lands, nothing drains, and the writer spins.
 *
 * TinyGB found this first and moved for the same reason - see the comment
 * above TG_AUDIO in its Makefile.n31 - and mpg123 and ffplay, which both use
 * alsa-lib, play correctly on the same hardware.
 *
 * It is worth being clear that this is not the same thing as the start
 * threshold that used to be set to a whole buffer here. That was a real
 * defect and it is fixed, but it could never have caused this: when the
 * hardware pointer never moves, no threshold is reachable. Fixing it removed
 * a bug and did not remove the symptom.
 *
 * Why it looks like this
 * ----------------------
 *
 * The rest of this file is a thousand lines of converter, ladder and
 * recovery logic that has been tuned against real hardware, and none of it
 * cares which library moves the bytes. So rather than edit thirteen call
 * sites, this presents the handful of tinyalsa entry points the file uses.
 * The names are tinyalsa's; the implementation is not. They are only defined
 * when tinyalsa itself is absent, so the two can never collide.
 *
 * snd_pcm_set_params does the part that had to be worked out by hand before:
 * it picks a period and buffer the device will accept and sets the start and
 * stop thresholds to match them, rather than to what was asked for.
 */

#define PCM_OUT            0
#define PCM_FORMAT_S16_LE  0
#define PCM_STATE_RUNNING  SND_PCM_STATE_RUNNING
#define PCM_STATE_DRAINING SND_PCM_STATE_DRAINING

#define PCM_PARAM_RATE     0
#define PCM_PARAM_CHANNELS 1

struct pcm_config {
    unsigned int channels, rate, period_size, period_count, format;
    unsigned int start_threshold, stop_threshold, silence_threshold, avail_min;
};

struct pcm {
    snd_pcm_t *h;
    unsigned int rate, channels;
    int ready;
};

/* The capability line on the error path. alsa-lib answers this from the
   refined hardware parameters rather than from a static range. */
struct pcm_params { unsigned int rmin, rmax, cmin, cmax; };

static struct pcm_params *pcm_params_get(unsigned int card, unsigned int device,
                                         unsigned int flags)
{
    char name[32];
    snd_pcm_hw_params_t *hw = 0;
    snd_pcm_t *h = 0;
    struct pcm_params *p;

    (void)flags;
    p = calloc(1, sizeof *p);
    if (!p) return 0;

    snprintf(name, sizeof name, "hw:%u,%u", card, device);
    if (snd_pcm_open(&h, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
        return p;                       /* zeros; the message says nothing */

    snd_pcm_hw_params_malloc(&hw);
    if (hw && snd_pcm_hw_params_any(h, hw) >= 0) {
        int dir = 0;
        snd_pcm_hw_params_get_rate_min(hw, &p->rmin, &dir);
        snd_pcm_hw_params_get_rate_max(hw, &p->rmax, &dir);
        snd_pcm_hw_params_get_channels_min(hw, &p->cmin);
        snd_pcm_hw_params_get_channels_max(hw, &p->cmax);
    }
    if (hw) snd_pcm_hw_params_free(hw);
    snd_pcm_close(h);
    return p;
}

static unsigned int pcm_params_get_min(struct pcm_params *p, unsigned int what)
{
    if (!p) return 0;
    return what == PCM_PARAM_RATE ? p->rmin : p->cmin;
}

static unsigned int pcm_params_get_max(struct pcm_params *p, unsigned int what)
{
    if (!p) return 0;
    return what == PCM_PARAM_RATE ? p->rmax : p->cmax;
}

static void pcm_params_free(struct pcm_params *p) { free(p); }

static struct pcm *pcm_open(unsigned int card, unsigned int device,
                            unsigned int flags, struct pcm_config *cfg)
{
    char name[32];
    struct pcm *s;
    unsigned int us;
    int rc;

    (void)flags;
    s = calloc(1, sizeof *s);
    if (!s) return 0;

    snprintf(name, sizeof name, "hw:%u,%u", card, device);
    if (snd_pcm_open(&s->h, name, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        s->h = 0;
        return s;                       /* not ready; the caller reports it */
    }

    /*
     * The buffer, expressed as the time it holds rather than as a frame
     * count, because that is what set_params takes and what the shape ladder
     * above actually means. Zero for the resample argument: if this rate is
     * not available the call fails and the ladder moves on, rather than
     * quietly inserting a converter and playing at the wrong pitch.
     */
    us = cfg->rate ? (unsigned int)((unsigned long long)cfg->period_size
                                    * cfg->period_count * 1000000ull / cfg->rate)
                   : 200000u;
    if (us < 20000u) us = 20000u;

    rc = snd_pcm_set_params(s->h, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            cfg->channels, cfg->rate, 0, us);
    if (rc < 0) {
        snd_pcm_close(s->h);
        s->h = 0;
        return s;
    }

    s->rate = cfg->rate;
    s->channels = cfg->channels;
    s->ready = 1;
    return s;
}

static int pcm_is_ready(struct pcm *s) { return s && s->ready; }
static unsigned int pcm_get_rate(struct pcm *s) { return s ? s->rate : 0; }
static unsigned int pcm_get_channels(struct pcm *s) { return s ? s->channels : 0; }

/*
 * Frames written, or negative. Underruns are recovered here and are NOT
 * hidden: snd_pcm_recover returns zero when it fixed one, which is the signal
 * the caller uses to count them. tinyalsa recovered silently and returned
 * success, which is the reason a stream restarting several times a second
 * looked exactly like one playing cleanly.
 */
static int pcm_writei(struct pcm *s, const void *data, unsigned int frames)
{
    snd_pcm_sframes_t n;

    if (!s || !s->h) return -1;

    n = snd_pcm_writei(s->h, data, frames);
    if (n < 0) {
        if (snd_pcm_recover(s->h, (int)n, 1 /* silent */) < 0)
            return -1;
        n = snd_pcm_writei(s->h, data, frames);
        if (n < 0) return -1;
    }
    return (int)n;
}

static int pcm_state(struct pcm *s)
{
    return (s && s->h) ? (int)snd_pcm_state(s->h) : -1;
}

static int pcm_prepare(struct pcm *s)
{
    return (s && s->h) ? snd_pcm_prepare(s->h) : -1;
}

/* Drop rather than drain: every caller of this wants the buffer discarded -
   a seek, a stop, a track change - and draining would play out audio the
   listener has already moved past. */
static int pcm_stop(struct pcm *s)
{
    return (s && s->h) ? snd_pcm_drop(s->h) : -1;
}

static int pcm_close(struct pcm *s)
{
    if (!s) return -1;
    if (s->h) snd_pcm_close(s->h);
    free(s);
    return 0;
}
#endif /* TINYPOD_HAVE_ALSALIB */

/*
 * Either library. The shim above gives alsa-lib the same entry points
 * tinyalsa has, so every path below this line works through whichever one was
 * compiled in and none of them needs to know which.
 */
#if defined(TINYPOD_HAVE_TINYALSA) || defined(TINYPOD_HAVE_ALSALIB)
#define TINYPOD_HAVE_ALSA 1
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
#ifdef TINYPOD_HAVE_ALSA
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
#ifdef TP_WITH_SOXR
    soxr_t   soxr;           /* VHQ, created on first use */
#endif
    float   *taps;           /* SRC_PHASES x SRC_TAPS, built at first use */
    float    hist[2][32];    /* SRC_TAPS of input history, per channel */
    int      primed;         /* input samples seen, until the window is full */
    uint32_t pos_q16;        /* fractional position, carried across blocks */
    int16_t *mix;            /* remixed input, when the counts differ */
    size_t   mix_frames;
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
#if defined(TINYPOD_HAVE_ALSA) || defined(TINYPOD_HAVE_OSS)
    return 1;
#else
    return 0;
#endif
}

/* ---------------------------------------------------------------- ALSA --- */

static struct tp_sink *alsa_open(int rate, int channels, char *err, size_t errsz)
{
#ifdef TINYPOD_HAVE_ALSA
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
    /*
     * The two knobs the comment above promises.
     *
     * They were documented, declared, and never assigned - so the documented
     * way to try a buffer shape on the device without a rebuild did nothing
     * at all, silently, which is worse than not offering one. Anybody using
     * them to narrow a problem would have concluded the shape made no
     * difference.
     */
    unsigned int want_ms = 0, want_count = 0;

    {
        const char *e = getenv("TINYPOD_ALSA_PERIOD_MS");
        if (e && *e) want_ms = (unsigned int)strtoul(e, NULL, 10);
        e = getenv("TINYPOD_ALSA_PERIODS");
        if (e && *e) want_count = (unsigned int)strtoul(e, NULL, 10);

        /* Nonsense is ignored rather than passed to the driver: a period of
           zero divides by zero further down, and a thousand periods is not a
           shape anybody meant to ask for. */
        if (want_ms > 1000u) want_ms = 0;
        if (want_count > 64u) want_count = 0;
    }
    unsigned int i, n = sizeof SHAPES / sizeof SHAPES[0];
    char tried[160] = "";
    char cando[128] = "";

    /*
     * What the device says it can do, for the error message only.
     *
     * It is a range, and hardware rates are not a range: a codec can report
     * 8000-96000 and accept only the 48 kHz family. Reading two numbers off
     * this and concluding that 44100 is fine is how we ended up asking for a
     * rate the hardware does not have, and then reporting that the device
     * would not open. So it goes in the message and nowhere else.
     */
    {
        struct pcm_params *pp = pcm_params_get(card, device, PCM_OUT);

        if (pp) {
            snprintf(cando, sizeof cando,
                     "device does %u-%u Hz, %u-%u channels",
                     pcm_params_get_min(pp, PCM_PARAM_RATE),
                     pcm_params_get_max(pp, PCM_PARAM_RATE),
                     pcm_params_get_min(pp, PCM_PARAM_CHANNELS),
                     pcm_params_get_max(pp, PCM_PARAM_CHANNELS));
            pcm_params_free(pp);
        }
    }

    /*
     * The rates to try, in order.
     *
     * The one the hardware actually clocks comes first, and that is not the
     * same question as which ones it accepts. Measured on this device, asked
     * for a 44.1 kHz track:
     *
     *     rate: 44100        accepted in hw_params
     *     state: XRUN
     *     hw_ptr      : 0    and never advanced a frame
     *     appl_ptr    : 1024
     *
     * The driver does not constrain the rate even though the codec runs only
     * at 48 kHz, so a successful open proves nothing about whether anything
     * will come out. 48000 is what a 12 MHz master clock divides into, and
     * soxr makes the conversion cost nothing worth hearing.
     *
     * The track's own rate is still tried, second, so hardware that does
     * clock it is not resampled for no reason. TINYPOD_ALSA_RATE=track puts
     * it back in front for a driver that reports honestly, and a number
     * pins one rate outright.
     */
    static const int RATES[] = { 48000, 0, 44100, 32000, 96000, 88200,
                                 24000, 22050, 16000, 8000 };
    static const int RATES_TRACK_FIRST[] = { 0, 44100, 48000, 32000, 96000,
                                             88200, 24000, 22050, 16000, 8000 };
    const int *rates = RATES;
    unsigned int r, nr = sizeof RATES / sizeof RATES[0];
    int pinned = 0;

    if ((e = getenv("TINYPOD_ALSA_RATE")) != NULL && *e) {
        if (!strcmp(e, "track")) {
            rates = RATES_TRACK_FIRST;
        } else {
            pinned = (int)strtol(e, NULL, 10);
            if (pinned > 0)
                nr = 1;
        }
    }

    /*
     * Channels the same way. A device that only does stereo refuses a mono
     * open outright, and a mono track is not rare enough to lose - the remix
     * on the way in costs a copy.
     */
    int chans[2] = { channels, channels == 2 ? 0 : 2 };
    unsigned int c;

    for (r = 0; r < nr; r++) {
        int want;

        if (pinned > 0) {
            want = pinned;
        } else {
            want = rates[r] ? rates[r] : rate;
            /* The track rate appears once, wherever the zero sits; skip a
               later entry that names the same number. */
            if (rates[r] && rates[r] == rate)
                continue;
        }
        if (want <= 0)
            continue;

      for (c = 0; c < 2; c++) {
        int wantc = chans[c];

        if (wantc <= 0)
            continue;

        for (i = 0; i < n; i++) {
            unsigned int ms = want_ms ? want_ms : SHAPES[i].ms;
            unsigned int count = want_count ? want_count : SHAPES[i].count;
            size_t used = strlen(tried);

            /* A whole number of frames, or a power of two if the rate will
               not divide evenly - 44100 gives 882 at 20 ms. */
            period = ((unsigned int)want * ms) / 1000u;
            if (period == 0 || ((unsigned int)want * ms) % 1000u)
                period = 1024u;

            memset(&cfg, 0, sizeof(cfg));
            cfg.channels = (unsigned int)wantc;
            cfg.rate = (unsigned int)want;
            cfg.format = PCM_FORMAT_S16_LE;
            cfg.period_size = period;
            cfg.period_count = count;
            /*
             * One period, and NOT the whole buffer.
             *
             * This asked for the whole buffer, on the reasoning that starting
             * full buys the most time before the first underrun. On the codec
             * that worked. On snd-aloop it hung: the stream stayed PREPARED
             * for ever, one write landed, and the writer spun at ninety per
             * cent of the CPU with the hardware pointer never moving.
             *
             * The reason is in tinyalsa, and it is worth writing down because
             * it will bite anything else that sets this field. pcm_open reads
             * the REFINED period size and count back from the driver - the
             * driver is allowed to change them, and snd-aloop does - but then
             * computes the buffer from the ones that were ASKED for:
             *
             *     pcm->config.period_size  = param_get_int(... PERIOD_SIZE);
             *     pcm->config.period_count = param_get_int(... PERIODS);
             *     pcm->buffer_size = config->period_count * config->period_size;
             *
             * So pcm->buffer_size can be larger than the buffer that actually
             * exists. The write path then starts the stream when
             *
             *     pcm->buffer_size - avail >= start_threshold
             *
             * and with start_threshold equal to that same inflated figure,
             * the test needs avail to reach zero against a buffer bigger than
             * the real one. It never can. The stream never starts, nothing
             * drains, and the next write has nowhere to go.
             *
             * One period is reachable whatever the driver refines to, which
             * is also what tinyalsa defaults to when this field is left zero
             * and why tinyplay reaches RUNNING on the same loopback.
             *
             * The protection against underruns was never really coming from
             * the threshold anyway - it comes from the buffer being 320 ms
             * instead of 40 ms, and that is unchanged. All this delayed was
             * the moment the first sample was allowed out.
             */
            cfg.start_threshold = period;
            cfg.stop_threshold = period * count;
            cfg.silence_threshold = 0;
            cfg.avail_min = period;

            s->pcm = pcm_open(card, device, PCM_OUT, &cfg);
            if (s->pcm && pcm_is_ready(s->pcm)) {
                unsigned int got = pcm_get_rate(s->pcm);
                unsigned int gotc = pcm_get_channels(s->pcm);

                /*
                 * What it gave us, not what we asked for. A driver that
                 * accepts a rate and then clocks a different one plays
                 * everything at the wrong speed, and the converter can only
                 * correct for it if it is told.
                 */
                s->rate = got ? (int)got : want;
                s->channels = gotc ? (int)gotc : wantc;
                s->period_frames = period;
                s->periods = count;
                return s;
            }
            snprintf(tried + used, sizeof(tried) - used, "%s%u@%ux%d",
                     used ? ", " : "", count, (unsigned)want, wantc);
            if (s->pcm) {
                pcm_close(s->pcm);
                s->pcm = NULL;
            }
            if (want_ms || want_count)
                break;                   /* pinned by hand: one shape only */
        }
      }
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
 * A Kaiser-windowed sinc polyphase resampler.
 *
 * This exists because tinyalsa converts nothing: it opens hw:0,0 and hands
 * the hardware exactly what it is given, so a codec clocked at 48 kHz refuses
 * a 44.1 kHz track outright. alsa-lib's "default" is plughw and resamples in
 * userspace, which is why mpg123 and ffplay played tracks we would not.
 *
 * It was linear interpolation first, which is a two-tap triangular filter:
 * several dB of droop across the top of the band and images only about 25 dB
 * down. Audible, and not what to ship.
 *
 * Not libsoxr. The wide build already links swresample, so soxr would make
 * three resamplers in one tree; and the lean build lives in an initramfs,
 * which is a tmpfs resident for the whole session, where another library is a
 * permanent cost on a machine with 55 MiB. This is one implementation both
 * builds share, about a hundred lines, with its table built at open time
 * rather than compiled in.
 *
 * Shape: 32 taps, 256 phases. The taps are float and the device has VFPv3-D16,
 * so this is roughly 3M multiply-accumulates a second at 48 kHz stereo -
 * comfortably within budget on a Cortex-A8 that is otherwise waiting on NAND.
 *
 * Cutoff follows the direction. Upsampling keeps the whole source band
 * (fc = 1); downsampling lowers it to the destination's Nyquist so nothing
 * folds back. Each phase is normalised to unit sum, or the passband would sit
 * a fraction of a dB off flat and drift with the phase.
 */
/* Only compiled when soxr is not linked: it is the fallback, not a second
   engine to choose between at runtime. */
#ifndef TP_WITH_SOXR
#define SRC_TAPS   32
#define SRC_PHASES 256
#define SRC_CENTRE (SRC_TAPS / 2)

/* Modified Bessel function of the first kind, order 0, by its series. It
   converges quickly for the arguments a Kaiser window asks for. */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;

    for (k = 1; k < 32; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

static double sinc_pi(double x)
{
    if (x > -1e-9 && x < 1e-9)
        return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}

/*
 * Build the phase table. beta = 8.6 puts the stopband around -80 dB, which is
 * below the noise floor of anything 16-bit that reaches this.
 */
static int src_build(struct tp_sink *s)
{
    const double beta = 8.6;
    double fc = (double)s->rate / (double)s->src_rate;
    double i0b;
    unsigned ph;

    if (fc > 1.0)
        fc = 1.0;                 /* upsampling: keep the whole source band */

    s->taps = malloc(sizeof(float) * SRC_PHASES * SRC_TAPS);
    if (!s->taps)
        return -1;

    i0b = bessel_i0(beta);

    for (ph = 0; ph < SRC_PHASES; ph++) {
        double p = (double)ph / (double)SRC_PHASES;
        double sum = 0.0;
        int j;

        for (j = 0; j < SRC_TAPS; j++) {
            /* Distance from the output position to input sample j, in input
               samples. The output sits p past hist[SRC_CENTRE - 1]. */
            double x = ((double)SRC_CENTRE - 1.0 + p) - (double)j;
            double w = x / (double)SRC_CENTRE;      /* -1 .. 1 across the window */
            double t;

            if (w < -1.0 || w > 1.0) {
                s->taps[ph * SRC_TAPS + j] = 0.0f;
                continue;
            }
            t = fc * sinc_pi(fc * x) *
                bessel_i0(beta * sqrt(1.0 - w * w)) / i0b;
            s->taps[ph * SRC_TAPS + j] = (float)t;
            sum += t;
        }

        /* Unit gain at DC, per phase. */
        if (sum > 1e-9)
            for (j = 0; j < SRC_TAPS; j++)
                s->taps[ph * SRC_TAPS + j] /= (float)sum;
    }
    return 0;
}

static int16_t clamp16(float v)
{
    if (v > 32767.0f)  return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

/*
 * Convert one block. State carries across calls - the tap history and the
 * fractional position - because the decoder hands over arbitrary block sizes
 * and a filter that restarts at each block boundary clicks at every one.
 */
static int convert_builtin(struct tp_sink *s, const int16_t *in, int in_frames)
{
    uint32_t step;
    size_t need;
    int out = 0;
    int i, ch;

    if (in_frames <= 0)
        return 0;
    if (!s->taps && src_build(s) != 0)
        return -1;

    /* Input frames per output frame, Q16. */
    step = (uint32_t)(((uint64_t)s->src_rate << 16) / (uint32_t)s->rate);
    if (!step)
        step = 1;

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

    for (i = 0; i < in_frames; i++) {
        /* Already remixed to the device's channel count by convert(). */
        int16_t l = in[(size_t)i * s->channels];
        int16_t r = s->channels > 1 ? in[(size_t)i * s->channels + 1] : l;
        int j;

        /* Newest sample at the end; the window is centred SRC_CENTRE back. */
        for (j = 0; j < SRC_TAPS - 1; j++) {
            s->hist[0][j] = s->hist[0][j + 1];
            s->hist[1][j] = s->hist[1][j + 1];
        }
        s->hist[0][SRC_TAPS - 1] = (float)l;
        s->hist[1][SRC_TAPS - 1] = (float)r;

        if (s->primed < SRC_TAPS) {
            /* Still filling the window. Nothing to emit that would not be
               filtered against samples that do not exist yet. */
            s->primed++;
            continue;
        }

        while (s->pos_q16 < 65536u && (size_t)out < s->conv_frames) {
            const float *t = s->taps +
                (size_t)(s->pos_q16 >> (16 - 8)) % SRC_PHASES * SRC_TAPS;
            float acc[2] = { 0.0f, 0.0f };

            for (j = 0; j < SRC_TAPS; j++) {
                acc[0] += s->hist[0][j] * t[j];
                acc[1] += s->hist[1][j] * t[j];
            }
            for (ch = 0; ch < s->channels; ch++)
                s->conv[(size_t)out * s->channels + ch] =
                    clamp16(acc[ch > 1 ? 1 : ch]);
            out++;
            s->pos_q16 += step;
        }
        s->pos_q16 -= 65536u;
    }
    return out;
}
#endif /* !TP_WITH_SOXR */

#ifdef TP_WITH_SOXR
/*
 * libsoxr at VHQ.
 *
 * This is not an optimisation over the filter above; it is the reason the
 * filter above exists as a fallback rather than as the answer. soxr's VHQ
 * setting is flat to 20 kHz with images below -100 dB. The built-in one is
 * 32 taps and 256 phases, which is respectable and audibly worse.
 *
 * It matters more here than it would elsewhere: the codec clock is 12 MHz,
 * which divides into 48 kHz and never into 44.1 kHz, so nearly every track in
 * an iTunes library goes through this on its way out. It is not a fallback
 * path, it is the path.
 *
 * Interleaved int16 in and out, one soxr for the life of the sink, so its
 * filter state carries across blocks the way the built-in one's does.
 */
static int convert_soxr(struct tp_sink *s, const int16_t *in, int in_frames)
{
    size_t idone = 0, odone = 0;
    size_t need;

    if (!s->soxr) {
        soxr_io_spec_t io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
        /*
         * HQ, not VHQ. HQ is soxr's 20-bit setting and the sink is 16-bit,
         * so VHQ's extra eight bits of stopband go somewhere the hardware
         * cannot represent - and they are not free on a Cortex-A8 running
         * soxr's scalar engine, which is what this is: the SIMD ones are
         * built for a CPU this is not. Set TINYPOD_SOXR_QUALITY=vhq to try
         * it on the device, which is the only place the cost is real.
         */
        const char *qs = getenv("TINYPOD_SOXR_QUALITY");
        unsigned long recipe = SOXR_HQ;
        soxr_quality_spec_t q;
        soxr_error_t e = NULL;

        if (qs && strcmp(qs, "vhq") == 0)
            recipe = SOXR_VHQ;
        else if (qs && strcmp(qs, "mq") == 0)
            recipe = SOXR_MQ;
        q = soxr_quality_spec(recipe, 0);

        s->soxr = soxr_create((double)s->src_rate, (double)s->rate,
                              (unsigned)s->channels, &e, &io, &q, NULL);
        if (!s->soxr || e)
            return -1;
    }

    /* soxr can hold samples back, so ask for the ratio plus a margin rather
       than assuming a fixed relationship between in and out. */
    need = (size_t)((uint64_t)in_frames * (uint32_t)s->rate /
                    (uint32_t)s->src_rate) + 32;
    if (need > s->conv_frames) {
        int16_t *nb = realloc(s->conv, need * (size_t)s->channels *
                                       sizeof(int16_t));
        if (!nb)
            return -1;
        s->conv = nb;
        s->conv_frames = need;
    }

    if (soxr_process(s->soxr, in, (size_t)in_frames, &idone,
                     s->conv, s->conv_frames, &odone) != NULL)
        return -1;
    return (int)odone;
}
#endif

/*
 * Channels first, into what the device asked for.
 *
 * Kept separate from the rate change because soxr does not do it: it is
 * created for a channel count and expects that many in and out, so handing it
 * interleaved mono while telling it stereo makes it read one frame's left and
 * the next frame's right. Mono is duplicated, and anything wider than the
 * device takes is dropped to its first channels rather than folded down - a
 * fold needs gain decisions this has no business making.
 */
static const int16_t *remix(struct tp_sink *s, const int16_t *in, int frames)
{
    int i, ch;

    if (s->src_channels == s->channels)
        return in;

    if ((size_t)frames > s->mix_frames) {
        int16_t *nb = realloc(s->mix, (size_t)frames * (size_t)s->channels *
                                      sizeof(int16_t));
        if (!nb)
            return NULL;
        s->mix = nb;
        s->mix_frames = (size_t)frames;
    }

    for (i = 0; i < frames; i++) {
        for (ch = 0; ch < s->channels; ch++) {
            int src = ch < s->src_channels ? ch : s->src_channels - 1;
            s->mix[(size_t)i * s->channels + ch] =
                in[(size_t)i * s->src_channels + src];
        }
    }
    return s->mix;
}

/*
 * One conversion, whichever engine is compiled in. Callers - and the test -
 * do not choose, so what ships is what the tests ran against.
 */
static int convert(struct tp_sink *s, const int16_t *in, int in_frames)
{
    if (in_frames <= 0)
        return 0;

    in = remix(s, in, in_frames);
    if (!in)
        return -1;

    if (s->src_rate == s->rate) {
        /* Channels changed, rate did not. Nothing for a filter to do. */
        if ((size_t)in_frames > s->conv_frames) {
            int16_t *nb = realloc(s->conv, (size_t)in_frames *
                                           (size_t)s->channels *
                                           sizeof(int16_t));
            if (!nb)
                return -1;
            s->conv = nb;
            s->conv_frames = (size_t)in_frames;
        }
        memcpy(s->conv, in,
               (size_t)in_frames * (size_t)s->channels * sizeof(int16_t));
        return in_frames;
    }

#ifdef TP_WITH_SOXR
    return convert_soxr(s, in, in_frames);
#else
    return convert_builtin(s, in, in_frames);
#endif
}

/*
 * The same conversion the sink runs, reachable from a test.
 *
 * It only happens on a device whose codec refuses the track's rate - tinyalsa
 * converts nothing, unlike alsa-lib's plughw - which is not something a host
 * build can arrange. Without this the resampler would ship having never been
 * called once.
 *
 * A converter rather than a function, because the state is the interesting
 * part: both engines carry filter history across blocks, and soxr will not
 * return a sample until its filter has filled. A per-call hook tested neither
 * and quietly hid the latency.
 */
struct tp_sink *tp_sink_convert_open(int src_rate, int dst_rate,
                                     int src_ch, int dst_ch)
{
    struct tp_sink *s;

    if (src_rate <= 0 || dst_rate <= 0 || src_ch <= 0 || dst_ch <= 0)
        return NULL;

    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->kind = SINK_ALSA;      /* no pcm: close() checks before touching it */
    s->fd = -1;
    s->rate = dst_rate;
    s->channels = dst_ch;
    s->src_rate = src_rate;
    s->src_channels = src_ch;
    return s;
}

int tp_sink_convert_block(struct tp_sink *s, const int16_t *in, int in_frames,
                          int16_t *out, int out_cap_frames)
{
    int got;

    if (!s || !in || !out)
        return -1;

    got = convert(s, in, in_frames);
    if (got > out_cap_frames)
        got = out_cap_frames;
    if (got > 0)
        memcpy(out, s->conv,
               (size_t)got * (size_t)s->channels * sizeof(int16_t));
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

#ifdef TINYPOD_HAVE_ALSA
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
            snprintf(out, cap, "alsa %lu ms %d>%d %s", ms, s->src_rate, s->rate,
#ifdef TP_WITH_SOXR
                     "soxr"
#else
                     "sinc"
#endif
                     );
        else
            snprintf(out, cap, "alsa %lu ms", ms);
    } else {
        snprintf(out, cap, "alsa");
    }
    return 0;
}

void tp_sink_flush(struct tp_sink *s)
{
    if (!s)
        return;

#if defined(TP_WITH_SOXR)
    /*
     * The resampler holds filter state from the samples before the jump.
     * Clearing it costs one conversion's worth of warm-up and avoids a
     * transient at the seam.
     */
    if (s->soxr)
        soxr_clear(s->soxr);
#endif

#ifdef TINYPOD_HAVE_ALSA
    if (s->kind == SINK_ALSA && s->pcm) {
        /* stop drops what is queued; prepare makes the stream writable
           again. The pair is what pause/resume already does, and it is the
           only way tinyalsa offers to discard rather than play out. */
        pcm_stop(s->pcm);
        pcm_prepare(s->pcm);
        return;
    }
#endif
#ifdef TINYPOD_HAVE_OSS
    if (s->kind == SINK_OSS && s->fd >= 0)
        ioctl(s->fd, SNDCTL_DSP_RESET, 0);
#endif
}

void tp_sink_pause(struct tp_sink *s, int paused)
{
    if (!s)
        return;
#ifdef TINYPOD_HAVE_ALSA
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
#ifdef TINYPOD_HAVE_ALSA
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
#ifdef TINYPOD_HAVE_ALSA
    if (s->kind == SINK_ALSA && s->pcm)
        pcm_close(s->pcm);
    free(s->conv);
    free(s->mix);
    free(s->taps);
#ifdef TP_WITH_SOXR
    if (s->soxr)
        soxr_delete(s->soxr);
#endif
#endif
#ifdef TINYPOD_HAVE_OSS
    if (s->kind == SINK_OSS && s->fd >= 0)
        close(s->fd);
#endif
    free(s);
}
