#ifndef TP_APP_H
#define TP_APP_H

#include "tp_db.h"
#include "tp_player.h"
#include "tp_config.h"
#include "tp_mount.h"

#include <stdio.h>

struct tp_app {
    struct tp_volume vol;
    struct tp_library lib;
    struct tp_player *player;
    struct tp_play_queue queue;
    struct tp_config cfg;
    enum tp_player_backend backend;
    int loaded;
};

int tp_app_init(struct tp_app *app, const char *mount, enum tp_player_backend backend);
void tp_app_free(struct tp_app *app);
int tp_app_load(struct tp_app *app);

int tp_app_cmd_scan(struct tp_app *app);
int tp_app_cmd_list(struct tp_app *app, const char *filter);
int tp_app_cmd_libcheck(struct tp_app *app, int json);
int tp_app_cmd_export_json(struct tp_app *app, FILE *out);
int tp_app_cmd_export_m3u(struct tp_app *app, FILE *out);
int tp_app_cmd_play_id(struct tp_app *app, uint64_t id);
int tp_app_cmd_play_file(struct tp_app *app, const char *path);
int tp_app_cmd_stop(struct tp_app *app);
int tp_app_cmd_pause(struct tp_app *app);
int tp_app_cmd_resume(struct tp_app *app);
int tp_app_cmd_next(struct tp_app *app);
int tp_app_cmd_prev(struct tp_app *app);
int tp_app_cmd_shuffle(struct tp_app *app);
int tp_app_cmd_status(struct tp_app *app);
int tp_app_cmd_decode(struct tp_app *app, const char *path, const char *out);
int tp_app_cmd_search(struct tp_app *app, const char *q);
void tp_app_print_banner(struct tp_app *app);

#endif
