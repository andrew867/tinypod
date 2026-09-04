/*
 * tp_lv_input.c — see tp_lv_input.h.
 */

#include "tp_lv_input.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* From the N31 button drivers: gpio-s5l8740.c reports the volume keys and
   gpio-d1830.c the PMIC ones. Taken from the source rather than guessed. */
#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP   115
#define KEY_PLAYPAUSE  164
/* gpio-d1830 reports the Home button as KEY_HOMEPAGE. */
#define KEY_HOMEPAGE   172

/* Long enough that a normal press is never mistaken for it, short enough that
   holding for it does not feel like waiting. */
#define LONG_PRESS_MS 550

#define MAX_FDS 4

static int s_fd[MAX_FDS];
static int s_fds;

static int s_play_down;         /* PLAY is held */
static unsigned long s_play_at; /* since when */
static int s_play_fired;        /* and the long press already fired */

struct n31_input_event {
    long     sec;
    long     usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

static unsigned long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 1000UL + (unsigned long)(t.tv_nsec / 1000000);
}

int tp_lv_input_open(void)
{
    int i;

    for (i = 0; i < 12 && s_fds < MAX_FDS; i++) {
        char path[64];
        int fd;
        unsigned long types = 0;
        int req;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        /*
         * Ask which event TYPES the device reports, not which key codes. The
         * key bitmap's first word covers codes 0 to 31, and every key wanted
         * here is above 100 - so testing it rejects exactly the devices that
         * are needed.
         */
        req = (int)(0x80000000u | ((sizeof types) << 16) | ('E' << 8) | 0x20);
        if (ioctl(fd, req, &types) >= 0 && (types & (1u << 1)))   /* EV_KEY */
            s_fd[s_fds++] = fd;
        else
            close(fd);
    }

    return s_fds;
}

void tp_lv_input_close(void)
{
    while (s_fds > 0)
        close(s_fd[--s_fds]);
}

int tp_lv_input_holding(void)
{
    return s_play_down && !s_play_fired;
}

enum tp_lv_key tp_lv_input_poll(void)
{
    struct n31_input_event ev;
    int i;

    /*
     * The long press fires while the button is still down. Checked before
     * reading, so it happens on a poll with no input at all - which is the
     * common case, since nothing arrives between press and threshold.
     */
    if (s_play_down && !s_play_fired &&
        now_ms() - s_play_at >= LONG_PRESS_MS) {
        s_play_fired = 1;
        return TP_LV_BACK;
    }

    for (i = 0; i < s_fds; i++) {
        while (read(s_fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != 1)               /* EV_KEY */
                continue;

            if (ev.code == KEY_PLAYPAUSE) {
                if (ev.value == 1) {
                    s_play_down = 1;
                    s_play_at = now_ms();
                    s_play_fired = 0;
                } else if (ev.value == 0) {
                    int was = s_play_down && !s_play_fired;
                    s_play_down = 0;
                    /* Released before the threshold, so it was a tap. If the
                       long press already fired, the release means nothing. */
                    if (was)
                        return TP_LV_SELECT;
                }
                continue;
            }

            /*
             * HOME on the press, and never on auto-repeat.
             *
             * It pops a screen and can leave the app, so a held button must
             * not run up the stack and out of the program while a thumb rests
             * on it.
             */
            if (ev.code == KEY_HOMEPAGE) {
                if (ev.value == 1)
                    return TP_LV_HOME;
                continue;
            }

            /* Auto-repeat counts on the volume keys and nowhere else: holding
               one should run down a long list rather than tapping forty
               times. */
            if (ev.value != 1 && ev.value != 2)
                continue;

            if (ev.code == KEY_VOLUMEUP)
                return TP_LV_UP;
            if (ev.code == KEY_VOLUMEDOWN)
                return TP_LV_DOWN;
        }
    }

    return TP_LV_NONE;
}
