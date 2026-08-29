/*
 * Does starting playback still block the caller?
 *
 * tp_player_play_file waits 120 ms after starting the decode thread so that an
 * unplayable file reports as a failure from the call. The CLI needs that. A UI
 * does not, and paying it on every track change is a fifth of a second of not
 * redrawing each time - so the UI sets async and the wait is skipped.
 *
 * This measures both, rather than trusting that the branch is taken. The fixture
 * is a real file, and on a machine with no usable ALSA device the decode thread
 * fails almost immediately - which is the case that makes the difference
 * visible: without async the call sits out the full wait to collect that
 * failure, with async it returns at once and the failure is collected later.
 */
#include "tp_player.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static long ms_since(struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000L +
           (t1.tv_nsec - t0->tv_nsec) / 1000000L;
}

static long time_play(int async, const char *path)
{
    struct tp_player *p = tp_player_create(TP_PLAYER_ALSA);
    struct timespec t0;
    long ms;

    if (!p) return -1;
    tp_player_set_async(p, async);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    tp_player_play_file(p, path);
    ms = ms_since(&t0);

    tp_player_stop(p);
    tp_player_destroy(p);
    return ms;
}

int main(void)
{
    const char *path =
        "tests/fixtures/sqlite-itdb-small/iPod_Control/Music/F13/QRLF.mp3";
    long sync_ms, async_ms;
    int fails = 0;

    printf("playback start:\n");

    sync_ms = time_play(0, path);
    async_ms = time_play(1, path);

    printf("  blocking start took %ld ms\n", sync_ms);
    printf("  async start took    %ld ms\n", async_ms);

    /*
     * The blocking path is allowed to be quick if the decode thread happens to
     * still be running - what must not happen is the async path paying a wait
     * it was told to skip. A UI frame is 33 ms; a start that fits inside one is
     * a start nobody sees.
     */
    if (async_ms > 30) {
        printf("  FAIL: async start blocked for %ld ms\n", async_ms);
        fails++;
    } else {
        printf("  ok: async start does not block the caller\n");
    }

    printf(fails ? "\nFAILED\n" : "\nall passed\n");
    return fails != 0;
}
