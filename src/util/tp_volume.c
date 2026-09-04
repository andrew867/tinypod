/*
 * tp_volume.c — see tp_volume.h.
 */

#include "tp_volume.h"

#include <stdio.h>
#include <string.h>

#ifdef TINYPOD_HAVE_TINYALSA
#include <tinyalsa/mixer.h>

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
