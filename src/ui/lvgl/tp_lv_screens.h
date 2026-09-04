/*
 * tp_lv_screens.h — the screens, separated from the run loop.
 *
 * Split out so the layout can be rendered headlessly: preview.c drives exactly
 * these functions with a stub library and writes PNGs, with no framebuffer and
 * no device. Every layout bug in the other three N31 apps was found that way
 * and none of them were visible in the source.
 */

#ifndef TP_LV_SCREENS_H
#define TP_LV_SCREENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TP_LV_W 240
#define TP_LV_H 432

struct tp_app;

/* What a list row shows. Filled in by the caller for each visible row only,
   so a library of ten thousand tracks costs the same as one of ten. */
struct tp_lv_row {
    const char *line1;
    const char *line2;
    const char *badge;     /* right-hand text, e.g. a duration or a count */
    bool        playing;   /* mark the row that is currently sounding */
};

/* Everything Now Playing draws. */
struct tp_lv_now {
    const char *title;
    const char *artist;
    const char *album;
    const char *codec;
    unsigned long pos_ms;
    unsigned long dur_ms;
    int  state;            /* 0 stopped, 1 playing, 2 paused */
    int  index, total;     /* position in the queue, 1-based; 0 total hides it */
    bool shuffle;
    /* "", "All" or "One". Setting repeat and getting no acknowledgement on
       the one screen that plays music made it feel like it had not taken. */
    const char *repeat;
    const char *error;     /* shown instead of the transport when set */
};

void tp_lv_screens_init(void);

/*
 * A list. `title` heads it, `count` is the whole list, `sel` and `top` are
 * indices into it, and `fill` is asked for the rows actually on screen.
 * `empty` is shown instead when count is zero.
 */
void tp_lv_show_list(const char *title, int count, int sel, int top,
                     void (*fill)(int index, struct tp_lv_row *out, void *ctx),
                     void *ctx, const char *empty);

/* How many rows a list shows at once, which the caller needs for scrolling. */
int tp_lv_list_rows(void);

/*
 * The status line: battery, whether there is a cable, and the clock.
 *
 * Nothing in this app showed either. The launcher has them, so the moment you
 * opened TinyPod you were blind to both - which matters most in the app you
 * leave running for hours.
 *
 * pct below zero means no battery was found. clock_valid is false when the
 * clock has never been set, and then hours/minutes are an uptime instead -
 * this device has no RTC that survives a power cycle, and a status bar
 * confidently showing 00:38 is worse than showing nothing.
 */
void tp_lv_set_status(int pct, bool plugged, bool clock_valid,
                      int hours, int minutes);

void tp_lv_show_now(const struct tp_lv_now *n);
void tp_lv_show_message(const char *title, const char *body);

/* Shown while the library loads. `pct` may be negative for a stage that cannot
   say how far along it is, and the bar is then hidden rather than shown at
   some arbitrary fill. Refreshes synchronously, because every caller is about
   to block. */
void tp_lv_show_scan(const char *where, const char *stage, int pct);

/* The bar at the bottom saying what the buttons do here. */
void tp_lv_set_hint(const char *hint);

/* Long-press feedback: PLAY is down and back is about to happen. */
void tp_lv_set_holding(bool on);

#endif
