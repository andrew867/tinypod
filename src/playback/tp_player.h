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

/*
 * Jump to `ms` into the track that is playing.
 *
 * Requested here and carried out on the decode thread, which is the only one
 * that may touch the file - so this returns as soon as the request is filed
 * and the position moves a moment later. Returns 0 if it was filed, -1 if
 * nothing is playing or the format cannot seek.
 */
int tp_player_seek_ms(struct tp_player *p, unsigned long ms);

/* Whether the track playing can be seeked at all, for a UI deciding whether
   to offer it. MP4 cannot; MP3, ADTS and WAV can. */
int tp_player_can_seek(struct tp_player *p);

/* Progress and format of the track being played. Zero when idle. */
unsigned long tp_player_position_ms(struct tp_player *p);

/* True once the audio device has taken data faster than it could possibly
   play it - which almost always means the stream is not running at all. The
   position is bounded by elapsed time, so the usual symptom of this (a clock
   racing) no longer shows; this is how it says so instead. */
int tp_player_not_pacing(struct tp_player *p);

/* What the audio sink turned out to be ("alsa 320 ms"), and how many times it
   has had to restart. An underrun is invisible from here - tinyalsa recovers
   by itself - but each one costs this codec a 60 ms settle, so a count is the
   difference between "it stutters" and a number. */
int tp_player_sink_desc(struct tp_player *p, char *out, size_t cap);
unsigned long tp_player_restarts(struct tp_player *p);
unsigned long tp_player_duration_ms(struct tp_player *p);
int tp_player_rate(struct tp_player *p);
int tp_player_channels(struct tp_player *p);
const char *tp_player_codec(struct tp_player *p);
/* Empty unless the last playback attempt failed. */
const char *tp_player_last_error(struct tp_player *p);
const char *tp_player_current_title(struct tp_player *p);

/*
 * What is queued up, and in what order.
 *
 * An entry is either a library track or a plain file. It has to be both,
 * because the folder browser plays things the database has never heard of -
 * and a queue of library ids alone was why picking a file from a folder left
 * the previous shuffle running underneath it, so Next jumped somewhere else
 * entirely and Now Playing named a track that was not the one you could hear.
 *
 * path is always set. id is the library track when there is one, and zero
 * when there is not; title is what to put on screen for a file that has no
 * database entry to ask.
 */
struct tp_queue_item {
    uint64_t id;
    char    *path;
    char    *title;
};

enum tp_repeat {
    TP_REPEAT_OFF = 0,   /* stop at the end of the queue */
    TP_REPEAT_ALL,       /* wrap round to the start */
    TP_REPEAT_ONE        /* the same track again */
};

/*
 * Shuffle is an ORDER over the items, not a permutation of them.
 *
 * Shuffling the array in place loses the album order for good, so turning
 * shuffle back off could not put it back - and the position was an index into
 * an array that had moved under it. order[] keeps the items where they are:
 * order[i] is which item plays i-th, pos is an index into order, and turning
 * shuffle off is rebuilding order as the identity while holding on to the
 * item that is playing.
 */
struct tp_play_queue {
    struct tp_queue_item *items;
    size_t  count;

    size_t *order;
    size_t  pos;         /* index into order, not into items */

    int shuffle;
    enum tp_repeat repeat;
};

void tp_queue_init(struct tp_play_queue *q);
void tp_queue_free(struct tp_play_queue *q);

/* The item that plays now, or NULL when the queue is empty. */
const struct tp_queue_item *tp_queue_current(const struct tp_play_queue *q);

/* Where in the running order it is, one-based, for "3 of 12". */
size_t tp_queue_index(const struct tp_play_queue *q);

int tp_queue_from_library(struct tp_play_queue *q, struct tp_library *lib, int shuffle);

/*
 * A drill-down as the queue: `idx` are indices into lib->tracks, in the order
 * the list showed them, and `start` is which of them was picked.
 *
 * Without this, choosing a track inside an album played it and then carried on
 * through whatever queue happened to exist - so the second track was rarely
 * the second track of the album.
 */
int tp_queue_from_indices(struct tp_play_queue *q, struct tp_library *lib,
                          const size_t *idx, size_t n, size_t start,
                          int shuffle);

/*
 * Move to a track already in the queue. Returns 0 if it was there, -1 if not,
 * which is how a caller tells "somewhere else in this album" from "somewhere
 * else entirely, build a new queue".
 */
int tp_queue_seek_id(struct tp_play_queue *q, uint64_t id);

/*
 * Everything playable in one folder, in name order, starting at `start`.
 *
 * `start` is matched by path; when it is not in the folder the queue still
 * builds and starts at the top. ids are filled in from the library where a
 * file happens to be in it, so a folder of tracks that ARE in the database
 * still shows their proper titles.
 */
int tp_queue_from_folder(struct tp_play_queue *q, struct tp_library *lib,
                         const char *folder, const char *start, int shuffle);

/* One file, and nothing after it. */
int tp_queue_from_file(struct tp_play_queue *q, struct tp_library *lib,
                       const char *path);

/*
 * Move. Returns -1 when there is nowhere to go, which is how the end of a
 * queue with repeat off stops playback instead of wrapping.
 *
 * next() honours TP_REPEAT_ONE; skip_next() is the same move with the button
 * pressed, where repeating one track would look like the button did nothing.
 */
int tp_queue_next(struct tp_play_queue *q);
int tp_queue_skip_next(struct tp_play_queue *q);
int tp_queue_prev(struct tp_play_queue *q);

/*
 * Turn shuffle on or off without losing your place: whatever is playing stays
 * playing, and becomes the position in the new order.
 */
void tp_queue_set_shuffle(struct tp_play_queue *q, int shuffle);

#endif
