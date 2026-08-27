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
enum tp_player_state tp_player_state(struct tp_player *p);
const char *tp_player_backend_name(enum tp_player_backend b);
enum tp_player_backend tp_player_backend_from_name(const char *name);

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
