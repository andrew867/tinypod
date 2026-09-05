/*
 * tp_volume.c — see tp_volume.h.
 */

#include "tp_volume.h"

#include <stdio.h>
#include <string.h>


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
#include <stdlib.h>

/*
 * tinyalsa's mixer, over alsa-lib's control interface.
 *
 * The same trick as the PCM shim in tp_sink.c and for the same reason: this
 * file is small and correct and none of it cares which library reaches the
 * control, so it keeps the names it was written against.
 *
 * snd_ctl rather than snd_mixer. tinyalsa's mixer IS the control interface -
 * a control found by its exact name, with a value per channel - whereas
 * alsa-lib's "simple mixer" is a higher-level abstraction over playback
 * elements that renames things and would not find a control called
 * "Headphone Digital Volume" by that name at all.
 */

struct mixer { snd_ctl_t *ctl; };

struct mixer_ctl {
    snd_ctl_t          *ctl;
    snd_ctl_elem_id_t  *id;
    long                min, max;
    unsigned int        count;
};

static struct mixer *mixer_open(unsigned int card)
{
    char name[16];
    struct mixer *m = calloc(1, sizeof *m);

    if (!m) return 0;
    snprintf(name, sizeof name, "hw:%u", card);
    if (snd_ctl_open(&m->ctl, name, 0) < 0) { free(m); return 0; }
    return m;
}

static void mixer_close(struct mixer *m)
{
    if (!m) return;
    if (m->ctl) snd_ctl_close(m->ctl);
    free(m);
}

static struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *m, const char *name)
{
    snd_ctl_elem_info_t *info = 0;
    struct mixer_ctl *c;

    if (!m || !m->ctl || !name) return 0;

    c = calloc(1, sizeof *c);
    if (!c) return 0;

    snd_ctl_elem_id_malloc(&c->id);
    snd_ctl_elem_info_malloc(&info);
    if (!c->id || !info) goto fail;

    snd_ctl_elem_id_set_interface(c->id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(c->id, name);

    snd_ctl_elem_info_set_id(info, c->id);
    if (snd_ctl_elem_info(m->ctl, info) < 0) goto fail;

    /*
     * The range comes from the control rather than being assumed to be
     * 0-100. tinyalsa's _percent calls do this conversion internally, and a
     * codec whose volume runs -63..0 would otherwise be set to silence by
     * every "percent" this file passes.
     */
    c->min = snd_ctl_elem_info_get_min(info);
    c->max = snd_ctl_elem_info_get_max(info);
    c->count = snd_ctl_elem_info_get_count(info);
    c->ctl = m->ctl;

    snd_ctl_elem_info_free(info);
    return c;

fail:
    if (info) snd_ctl_elem_info_free(info);
    if (c->id) snd_ctl_elem_id_free(c->id);
    free(c);
    return 0;
}

static unsigned int mixer_ctl_get_num_values(struct mixer_ctl *c)
{
    return c ? c->count : 0;
}

static int mixer_ctl_get_percent(struct mixer_ctl *c, unsigned int i)
{
    snd_ctl_elem_value_t *v = 0;
    long raw, span;
    int pct;

    if (!c || !c->ctl) return -1;

    snd_ctl_elem_value_malloc(&v);
    if (!v) return -1;
    snd_ctl_elem_value_set_id(v, c->id);
    if (snd_ctl_elem_read(c->ctl, v) < 0) {
        snd_ctl_elem_value_free(v);
        return -1;
    }
    raw = snd_ctl_elem_value_get_integer(v, i);
    snd_ctl_elem_value_free(v);

    span = c->max - c->min;
    if (span <= 0) return 0;
    pct = (int)(((raw - c->min) * 100 + span / 2) / span);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

static int mixer_ctl_set_percent(struct mixer_ctl *c, unsigned int i, int percent)
{
    snd_ctl_elem_value_t *v = 0;
    long span, raw;
    int rc;

    if (!c || !c->ctl) return -1;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    snd_ctl_elem_value_malloc(&v);
    if (!v) return -1;
    snd_ctl_elem_value_set_id(v, c->id);

    /* Read first: a control with several channels is written whole, and
       writing one channel of a stereo pair from a zeroed value would mute the
       other. */
    (void)snd_ctl_elem_read(c->ctl, v);

    span = c->max - c->min;
    raw = c->min + (span * percent + 50) / 100;
    snd_ctl_elem_value_set_integer(v, i, raw);

    rc = snd_ctl_elem_write(c->ctl, v);
    snd_ctl_elem_value_free(v);
    return rc < 0 ? -1 : 0;
}
#endif /* TINYPOD_HAVE_ALSALIB */

#ifdef TINYPOD_HAVE_TINYALSA
#include <tinyalsa/mixer.h>
#endif

#if defined(TINYPOD_HAVE_TINYALSA) || defined(TINYPOD_HAVE_ALSALIB)

static struct mixer     *s_mixer;
static struct mixer_ctl *s_ctl;
static char              s_name[64];
static int               s_tried;

/*
 * What to drive, in order of preference.
 *
 * The codec on this device calls it "Headphones Playback Volume". The others
 * are here so the same binary is useful on a machine whose codec named things
 * differently, rather than silently having no volume control.
 */
static const char *k_names[] = {
    "Headphones Playback Volume",
    "Headphone Playback Volume",
    "Master Playback Volume",
    "PCM Playback Volume",
    "Speaker Playback Volume"
};

int tp_volume_open(void)
{
    unsigned i;

    if (s_ctl)
        return 0;
    /* One attempt. A device with no mixer should not re-open the card on
       every redraw of the settings screen. */
    if (s_tried)
        return -1;
    s_tried = 1;

    s_mixer = mixer_open(0);
    if (!s_mixer)
        return -1;

    for (i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        s_ctl = mixer_get_ctl_by_name(s_mixer, k_names[i]);
        if (s_ctl) {
            snprintf(s_name, sizeof s_name, "%s", k_names[i]);
            return 0;
        }
    }

    mixer_close(s_mixer);
    s_mixer = NULL;
    return -1;
}

void tp_volume_close(void)
{
    if (s_mixer)
        mixer_close(s_mixer);
    s_mixer = NULL;
    s_ctl = NULL;
    s_tried = 0;
}

int tp_volume_available(void)
{
    return tp_volume_open() == 0;
}

const char *tp_volume_control_name(void)
{
    return s_ctl ? s_name : "";
}

int tp_volume_get(void)
{
    int v;

    if (tp_volume_open() != 0)
        return -1;

    /* Value 0 is the left channel. A stereo control is set to the same value
       on both here - a balance control is not what this is. */
    v = mixer_ctl_get_percent(s_ctl, 0);
    if (v < 0)
        return -1;
    return v > 100 ? 100 : v;
}

int tp_volume_set(int percent)
{
    unsigned n, i;

    if (tp_volume_open() != 0)
        return -1;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    n = mixer_ctl_get_num_values(s_ctl);
    for (i = 0; i < n; i++)
        mixer_ctl_set_percent(s_ctl, i, percent);

    /*
     * Read it back rather than trusting the write: the control's own range is
     * coarse (0..88 on this codec), so a percentage does not land exactly and
     * the UI should show where it actually went. Otherwise the number on
     * screen drifts away from the volume you can hear.
     */
    return tp_volume_get();
}

int tp_volume_step(int delta)
{
    int cur = tp_volume_get();
    int want;

    if (cur < 0)
        return -1;

    want = cur + delta;

    /*
     * Make sure a press always moves.
     *
     * The control has fewer steps than a percentage does, so rounding can put
     * a small delta back on the value it started from - and a volume key that
     * does nothing reads as broken. If the readback did not move, push
     * further in the same direction until it does.
     */
    for (;;) {
        int got = tp_volume_set(want);

        if (got < 0)
            return -1;
        if (got != cur || want <= 0 || want >= 100)
            return got;
        want += (delta >= 0) ? 1 : -1;
    }
}

#else /* no tinyalsa: a host build, where there is no card to drive */

int tp_volume_open(void) { return -1; }
void tp_volume_close(void) { }
int tp_volume_available(void) { return 0; }
const char *tp_volume_control_name(void) { return ""; }
int tp_volume_get(void) { return -1; }
int tp_volume_set(int percent) { (void)percent; return -1; }
int tp_volume_step(int delta) { (void)delta; return -1; }

#endif
