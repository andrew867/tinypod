#ifndef TP_PLAYER_H
#define TP_PLAYER_H

#include "tp_db.h"

enum tp_player_backend {
    TP_PLAYER_NULL = 0,
    TP_PLAYER_EXTERNAL,
    TP_PLAYER_ALSA
};

enum tp_player_state {
    TP_PLAYER_STOPPED = 0,
    TP_PLAYER_PLAYING,
    TP_PLAYER_PAUSED
};

struct tp_player;

struct tp_player *tp_player_create(enum tp_player_backend backend);
void tp_player_destroy(struct tp_player *p);

int tp_player_play_track(struct tp_player *p, const struct tp_track *t);
int tp_player_play_file(struct tp_player *p, const char *path);
int tp_player_pause(struct tp_player *p);
int tp_player_resume(struct tp_player *p);
int tp_player_stop(struct tp_player *p);

/*
 * Do not wait to find out whether playback started.
 *
 * Normally tp_player_play_file waits a moment after starting the decode thread
 * so that an unplayable file reports as a failure from the call rather than
 * silently. The CLI needs that - it exits immediately afterwards, and an error
 * discovered later would be an error nobody sees.
 *
 * A UI does not: it is still running, it polls the player anyway, and a fifth
 * of a second of not redrawing is a fifth of a second of looking broken on
 * every single track change. With this set, failures surface through
 * tp_player_last_error() and the state returning to STOPPED.
 */
void tp_player_set_async(struct tp_player *p, int async);
enum tp_player_state tp_player_state(struct tp_player *p);
const char *tp_player_backend_name(enum tp_player_backend b);
enum tp_player_backend tp_player_backend_from_name(const char *name);

/*
 * Block until the current track finishes, fails, or is stopped. Returns 0 if
 * it played to the end. The CLI needs this - without it "tinypod play" exits
 * the moment playback starts and takes the audio with it.
 */
int tp_player_wait(struct tp_player *p);

/* Progress and format of the track being played. Zero when idle. */
unsigned long tp_player_position_ms(struct tp_player *p);

/* True once the audio device has taken data faster than it could possibly
   play it - which almost always means the stream is not running at all. The
   position is bounded by elapsed time, so the usual symptom of this (a clock
   racing) no longer shows; this is how it says so instead. */
int tp_player_not_pacing(struct tp_player *p);
unsigned long tp_player_duration_ms(struct tp_player *p);
int tp_player_rate(struct tp_player *p);
int tp_player_channels(struct tp_player *p);
const char *tp_player_codec(struct tp_player *p);
/* Empty unless the last playback attempt failed. */
const char *tp_player_last_error(struct tp_player *p);
const char *tp_player_current_title(struct tp_player *p);

/* Queue helpers used by app */
struct tp_play_queue {
    uint64_t *ids;
    size_t count;
    size_t pos;
    int shuffle;
};

void tp_queue_init(struct tp_play_queue *q);
void tp_queue_free(struct tp_play_queue *q);
int tp_queue_from_library(struct tp_play_queue *q, struct tp_library *lib, int shuffle);
int tp_queue_next(struct tp_play_queue *q);
int tp_queue_prev(struct tp_play_queue *q);

#endif
