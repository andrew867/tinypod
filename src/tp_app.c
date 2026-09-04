#include "tp_app.h"
#include "fs/tp_browse.h"
#include "util/tp_badfile.h"
#include "util/tp_diag.h"
#include "tp_decode.h"
#include "tp_sink.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_path.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tp_app_init(struct tp_app *app, const char *mount, enum tp_player_backend backend)
{
    memset(app, 0, sizeof(*app));
    app->backend = backend;
    tp_library_init(&app->lib);
    tp_queue_init(&app->queue);
    tp_config_init(&app->cfg);
    tp_config_load(&app->cfg);
    /* The queue is what acts on repeat, so it has to be told at startup and
       not only when the settings screen is opened. */
    if (!strcmp(app->cfg.repeat, "one"))
        app->queue.repeat = TP_REPEAT_ONE;
    else if (!strcmp(app->cfg.repeat, "all"))
        app->queue.repeat = TP_REPEAT_ALL;
    else
        app->queue.repeat = TP_REPEAT_OFF;
    app->player = tp_player_create(backend);
    if (!app->player)
        return -1;
    tp_diag_stage("detecting the volume");
    if (tp_mount_detect(mount, &app->vol) != 0) {
        tp_error("Could not find an iPod volume.\n"
                 "Pass --mount /path (volume root or iPod_Control),\n"
                 "or set TINYPOD_MOUNT, or mount the disk at /mnt/disk.");
        /* Distinct from a real failure. The command-line tools treat it
           as fatal, because listing nothing is not a useful answer; the
           UIs start anyway and say it on screen, because an app that
           exits before drawing is indistinguishable from one that
           crashed. */
        app->no_volume = 1;
        return TP_APP_NO_VOLUME;
    }
    return 0;
}

void tp_app_free(struct tp_app *app)
{
    if (app->loaded) {
        free(app->cfg.last_mount);
        app->cfg.last_mount = tp_strdup(app->vol.mount_root);
        tp_config_save(&app->cfg);
    }
    tp_player_destroy(app->player);
    tp_queue_free(&app->queue);
    tp_library_free(&app->lib);
    tp_volume_free(&app->vol);
    tp_config_free(&app->cfg);
}

/* Where loading reports to. Null until set, every call guarded, so nothing
   here depends on a UI existing. */
static tp_load_progress_fn s_load_progress;

void tp_app_set_load_progress(struct tp_app *app, tp_load_progress_fn fn)
{
    (void)app;                 /* one app per process; kept for symmetry */
    s_load_progress = fn;
}

static void load_step(const char *stage, int pct)
{
    if (s_load_progress) s_load_progress(stage, pct);
}

int tp_app_load(struct tp_app *app)
{
    int rc;
    load_step("reading the database", -1);
    if (app->loaded) {
        tp_library_free(&app->lib);
        tp_library_init(&app->lib);
    }
    tp_diag_stage("reading the database");
    rc = tp_db_load(&app->lib, app->vol.mount_root, app->vol.ipod_control_root,
                    TP_DB_FORMAT_UNKNOWN);
    {
        /*
         * Which reader, in the stage trail. That file is what is left after a
         * crash or a kill, and "database read" alone does not say which of
         * five readers was in the middle of it.
         */
        char stage[96];

        snprintf(stage, sizeof stage, "database read (%s)",
                 tp_db_format_name(app->lib.format));
        tp_diag_stage(stage);
    }
    if (rc != 0) {
        /* With no volume there is no path to name, and printing
           "(null)" reads like a bug in the loader rather than a disk
           that is not mounted. */
        {
            /*
             * Say which reader failed and why, when it is known. This was one
             * sentence naming a directory, for five readers and a dozen
             * distinct faults - which is the absence of a diagnosis rather
             * than a short one.
             */
            char line[256];

            if (tp_io_err_format(&app->lib.io_err, line, sizeof line) == 0)
                tp_error("%s", line);
        }
        tp_error("Could not load the iPod music library under:\n  %s",
                 app->vol.ipod_control_root
                     ? app->vol.ipod_control_root
                     : "(no volume mounted)");
        return -1;
    }
    app->loaded = 1;
    tp_diag_stage("library ready");

    {
        char msg[96];
        snprintf(msg, sizeof(msg), "%zu tracks, %zu artists",
                 app->lib.track_count, app->lib.artist_count);
        load_step(msg, -1);
    }

    /* A hundred stat calls across a read-only vfat mount is not free, and it
       is the last thing between here and a usable screen, so it reports. */
    app->lib.health.music_folder_count = 0;
    {
        int i;
        for (i = 0; i < 100; i++) {
            char f[8];
            char *p;
            snprintf(f, sizeof(f), "F%02d", i);
            p = tp_path_join3(app->vol.ipod_control_root, "Music", f);
            if (p && tp_is_dir(p))
                app->lib.health.music_folder_count++;
            free(p);
            if ((i % 10) == 9)
                load_step("checking music folders", i + 1);
        }
    }
    load_step("ready", 100);
    return 0;
}

void tp_app_print_banner(struct tp_app *app)
{
    printf("TinyPod\n\n");
    printf("Volume:\n  %s\n\n", app->vol.mount_root);
    if (!app->loaded)
        return;
    printf("Library:\n");
    printf("  format: %s\n", tp_db_format_name(app->lib.format));
    printf("  tracks: %zu\n", app->lib.track_count);
    printf("  artists: %zu\n", app->lib.artist_count);
    printf("  albums: %zu\n", app->lib.album_count);
    printf("  missing files: %zu\n\n", app->lib.health.missing_files);
    printf("Ready.\n");
}

int tp_app_cmd_scan(struct tp_app *app)
{
    if (tp_app_load(app) != 0)
        return 1;
    tp_app_print_banner(app);
    return 0;
}

int tp_app_cmd_list(struct tp_app *app, const char *filter)
{
    size_t i;
    size_t shown = 0;
    if (!app->loaded && tp_app_load(app) != 0)
        return 1;
    printf("%-20s %-22s %-22s %s\n", "TRACK_ID", "Artist", "Album", "Title");
    for (i = 0; i < app->lib.track_count; i++) {
        struct tp_track *t = &app->lib.tracks[i];
        if (filter && filter[0]) {
            if ((!t->title || !strstr(t->title, filter)) &&
                (!t->artist || !strstr(t->artist, filter)) &&
                (!t->album || !strstr(t->album, filter)))
                continue;
        }
        printf("%-20llu %-22.22s %-22.22s %s\n", (unsigned long long)t->track_id,
               t->artist ? t->artist : "", t->album ? t->album : "",
               t->title ? t->title : "");
        shown++;
        if (shown >= 5000)
            break;
    }
    printf("\n(%zu tracks)\n", app->lib.track_count);
    return 0;
}

int tp_app_cmd_search(struct tp_app *app, const char *q)
{
    size_t i;
    char *ql;
    size_t n;
    if (!app->loaded && tp_app_load(app) != 0)
        return 1;
    if (!q)
        q = "";
    n = strlen(q);
    ql = (char *)malloc(n + 1);
    if (!ql)
        return 1;
    for (i = 0; i < n; i++)
        ql[i] = (char)tolower((unsigned char)q[i]);
    ql[n] = '\0';
    printf("%-20s  %s\n", "TRACK_ID", "Artist - Title");
    for (i = 0; i < app->lib.track_count; i++) {
        struct tp_track *t = &app->lib.tracks[i];
        char hay[1024];
        char *h;
        size_t j;
        snprintf(hay, sizeof(hay), "%s %s %s", t->artist ? t->artist : "",
                 t->album ? t->album : "", t->title ? t->title : "");
        for (j = 0; hay[j]; j++)
            hay[j] = (char)tolower((unsigned char)hay[j]);
        h = strstr(hay, ql);
        if (!h)
            continue;
        printf("%llu  %s - %s\n", (unsigned long long)t->track_id,
               t->artist ? t->artist : "?", t->title ? t->title : "?");
    }
    free(ql);
    return 0;
}

int tp_app_cmd_libcheck(struct tp_app *app, int json)
{
    struct tp_library_health *h;
    size_t i;
    if (!app->loaded && tp_app_load(app) != 0)
        return 1;
    tp_db_validate(&app->lib);
    h = &app->lib.health;
    if (json) {
        printf("{\"mount\":\"%s\",\"format\":\"%s\",\"tracks\":{\"total\":%zu,"
               "\"playable\":%zu,\"missing_files\":%zu,\"unsupported_codec\":%zu,"
               "\"duplicate_ids\":%zu},\"paths\":{\"exact\":%zu,\"casefold\":%zu,"
               "\"outside\":%zu},\"music_folders\":%zu}\n",
               app->vol.mount_root, h->format_name, h->track_total, h->track_playable,
               h->missing_files, h->unsupported_codec, h->duplicate_ids, h->path_exact,
               h->path_casefold, h->path_outside, h->music_folder_count);
        return 0;
    }
    printf("TinyPod Library Check\n\n");
    printf("Volume:\n  %s\n\n", app->vol.mount_root);
    printf("Database:\n  format: %s\n  parse: OK\n\n", h->format_name);
    printf("Tracks:\n");
    printf("  total: %zu\n", h->track_total);
    printf("  playable: %zu\n", h->track_playable);
    printf("  missing files: %zu\n", h->missing_files);
    printf("  unsupported codec: %zu\n", h->unsupported_codec);
    /* Separately from missing and from unsupported: a file that is
       there and will not read is a storage problem, not a library one. */
    printf("  unreadable files: %zu\n", h->unreadable_files);
    printf("  duplicate IDs: %zu\n\n", h->duplicate_ids);
    printf("Paths:\n");
    printf("  exact matches: %zu\n", h->path_exact);
    printf("  casefold matches: %zu\n", h->path_casefold);
    printf("  outside mount rejected: %zu\n\n", h->path_outside);
    printf("Music folders: %zu\n", h->music_folder_count);
    for (i = 0; i < 100 && i < h->music_folder_count + 50; i++) {
        char f[8];
        char *p;
        size_t cnt = 0;
        DIR *d;
        struct dirent *de;
        snprintf(f, sizeof(f), "F%02d", (int)i);
        p = tp_path_join3(app->vol.ipod_control_root, "Music", f);
        if (!p || !tp_is_dir(p)) {
            free(p);
            continue;
        }
        d = opendir(p);
        if (d) {
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] != '.')
                    cnt++;
            }
            closedir(d);
            printf("  %s: %zu\n", f, cnt);
        }
        free(p);
    }
    return 0;
}

int tp_app_cmd_export_json(struct tp_app *app, FILE *out)
{
    size_t i;
    if (!app->loaded && tp_app_load(app) != 0)
        return 1;
    fprintf(out, "[\n");
    for (i = 0; i < app->lib.track_count; i++) {
        struct tp_track *t = &app->lib.tracks[i];
        char title[1024], artist[1024], album[1024], ip[1024], ap[1024];
        tp_json_escape(title, sizeof(title), t->title);
        tp_json_escape(artist, sizeof(artist), t->artist);
        tp_json_escape(album, sizeof(album), t->album);
        tp_json_escape(ip, sizeof(ip), t->ipod_path);
        tp_json_escape(ap, sizeof(ap), t->absolute_path);
        fprintf(out,
                "  {\"track_id\":%llu,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
                "\"duration_ms\":%u,\"ipod_path\":\"%s\",\"absolute_path\":\"%s\","
                "\"file_exists\":%s,\"source\":\"%s\"}%s\n",
                (unsigned long long)t->track_id, title, artist, album, t->duration_ms, ip,
                ap, t->file_exists ? "true" : "false", tp_source_name(t->source),
                (i + 1 < app->lib.track_count) ? "," : "");
    }
    fprintf(out, "]\n");
    return 0;
}

int tp_app_cmd_export_m3u(struct tp_app *app, FILE *out)
{
    size_t i;
    if (!app->loaded && tp_app_load(app) != 0)
        return 1;
    fprintf(out, "#EXTM3U\n");
    for (i = 0; i < app->lib.track_count; i++) {
        struct tp_track *t = &app->lib.tracks[i];
        if (!t->file_exists || !t->absolute_path)
            continue;
        fprintf(out, "#EXTINF:%u,%s - %s\n%s\n", t->duration_ms / 1000,
                t->artist ? t->artist : "Unknown", t->title ? t->title : "Unknown",
                t->absolute_path);
    }
    return 0;
}

/*
 * Play what the queue is pointing at.
 *
 * A library track goes through play_track so it gets its metadata; anything
 * else is a path and goes straight to the file. Everything that moves through
 * the queue comes here, so there is one place that knows the difference.
 */
int tp_app_play_current(struct tp_app *app)
{
    const struct tp_queue_item *it = tp_queue_current(&app->queue);
    struct tp_track *t;

    if (!it)
        return 1;

    /*
     * Chosen deliberately, so try it whichever list it is on.
     *
     * step_to_playable skips what is known bad when it is MOVING through a
     * queue; picking a row is a different intent, and on a volume that fails
     * intermittently it is also the only way back for a file that has since
     * become readable. Success below clears the mark.
     */
    if (it->id && (t = tp_library_find_track(&app->lib, it->id)) != NULL) {
        app->cfg.last_track_id = t->track_id;
        tp_config_save(&app->cfg);
        if (tp_player_play_track(app->player, t) != 0)
            return 1;
        tp_badfile_clear(it->path);
        return 0;
    }

    if (tp_player_play_file(app->player, it->path) != 0)
        return 1;
    tp_badfile_clear(it->path);
    return 0;
}

int tp_app_cmd_play_id(struct tp_app *app, uint64_t id)
{
    struct tp_track *t;
    if (!app->loaded && tp_app_load(app) != 0)
        return 1;
    t = tp_library_find_track(&app->lib, id);
    if (!t) {
        /* try match by short id mod 1e6 for convenience */
        size_t i;
        for (i = 0; i < app->lib.track_count; i++) {
            if ((app->lib.tracks[i].track_id % 1000000ull) == id) {
                t = &app->lib.tracks[i];
                break;
            }
        }
    }
    if (!t) {
        tp_error("No track with id %llu", (unsigned long long)id);
        return 1;
    }
    /*
     * Already in the queue: move to it and keep the context. Only build a new
     * queue when it is not, because rebuilding on every play is what made
     * choosing a track inside an album drop you back into the whole library.
     */
    if (tp_queue_seek_id(&app->queue, id) != 0) {
        tp_queue_from_library(&app->queue, &app->lib, app->cfg.shuffle);
        tp_queue_seek_id(&app->queue, id);
    }
    return tp_app_play_current(app);
}

int tp_app_cmd_play_file(struct tp_app *app, const char *path)
{
    char folder[TP_BROWSE_PATH_MAX];

    if (!path)
        return 1;

    /*
     * A file picked in the browser queues its whole folder.
     *
     * Which is what picking a track in a folder of music means - the album
     * carries on. It used to play the one file and leave whatever queue was
     * there, so the next track came from a shuffle of the entire library.
     *
     * The folder is the queue even with shuffle on; shuffle decides the order
     * within it, and the file that was picked still plays first.
     */
    if (tp_browse_parent(folder, sizeof folder, path) != 0 ||
        tp_queue_from_folder(&app->queue, &app->lib, folder, path,
                             app->cfg.shuffle) != 0) {
        if (tp_queue_from_file(&app->queue, &app->lib, path) != 0)
            return tp_player_play_file(app->player, path) == 0 ? 0 : 1;
    }

    return tp_app_play_current(app);
}

int tp_app_cmd_stop(struct tp_app *app)
{
    return tp_player_stop(app->player) == 0 ? 0 : 1;
}

int tp_app_cmd_pause(struct tp_app *app)
{
    return tp_player_pause(app->player) == 0 ? 0 : 1;
}

int tp_app_cmd_resume(struct tp_app *app)
{
    return tp_player_resume(app->player) == 0 ? 0 : 1;
}

/*
 * Move, and keep moving until the queue is pointing at something worth
 * trying.
 *
 * A file that would not read is stepped over rather than played and failed
 * again. The volume behind this player returns read errors on whole regions,
 * so a bad file is an ordinary event and stopping on one would make the
 * player unusable - but retrying one every time round a repeating queue is
 * just as bad, only noisier.
 *
 * Bounded by the length of the queue, which is what makes a queue where
 * NOTHING reads terminate: every entry is tried once and then there is
 * nowhere left to go, and playback stops having genuinely run out rather than
 * spinning at full tilt for as long as the battery lasts.
 */
static int step_to_playable(struct tp_play_queue *q,
                            int (*step)(struct tp_play_queue *))
{
    size_t tried;

    for (tried = 0; tried < q->count; tried++) {
        const struct tp_queue_item *it;

        if (step(q) != 0)
            return -1;

        it = tp_queue_current(q);
        if (!it || !it->path || !tp_badfile_is_bad(it->path))
            return 0;
    }
    return -1;
}

/*
 * Nothing queued and nothing said about what to queue: fall back to the
 * library, loading it if that has not happened yet.
 *
 * This is the ONLY place playback is allowed to care whether the database
 * loaded. Next, Previous and advance each used to open with
 *
 *     if (!app->loaded && tp_app_load(app) != 0) return 1;
 *
 * which meant a folder queue - built, in memory, needing no database at all -
 * could not be moved through on a device whose iTunes database was missing or
 * unreadable. On this hardware that is not a corner case: the database sits on
 * the volume that returns read errors, so the common failure took the
 * transport controls down with it.
 */
static int fall_back_to_library(struct tp_app *app)
{
    if (!app->loaded && tp_app_load(app) != 0)
        return -1;
    return tp_queue_from_library(&app->queue, &app->lib, app->cfg.shuffle);
}

/*
 * How far into a track Previous stops meaning "the one before".
 *
 * Every player anyone has used restarts the current track when you are a few
 * seconds in, and it is the behaviour that makes a mis-press recoverable:
 * pressing Previous once during a song you are enjoying should not lose it.
 */
#define TP_PREV_RESTART_MS 3000

/*
 * Next, as the button means it.
 *
 * Returns 1 at the end of a queue with repeat off - the caller stops rather
 * than wrapping, which is the whole difference between Repeat All and Off.
 */
int tp_app_cmd_next(struct tp_app *app)
{
    if (app->queue.count == 0 && fall_back_to_library(app) != 0)
        return 1;
    if (step_to_playable(&app->queue, tp_queue_skip_next) != 0)
        return 1;
    return tp_app_play_current(app);
}

/* Next, as a track ENDING means it - this is the one Repeat One acts on. */
int tp_app_cmd_advance(struct tp_app *app)
{
    /* No library fallback here on purpose. A track ending is not a request to
       start playing the whole device; if there is no queue there is nothing
       this was continuing. */
    if (app->queue.count == 0)
        return 1;
    if (step_to_playable(&app->queue, tp_queue_next) != 0)
        return 1;
    return tp_app_play_current(app);
}

int tp_app_cmd_prev(struct tp_app *app)
{
    if (app->queue.count == 0 && fall_back_to_library(app) != 0)
        return 1;

    /* Far enough in that you meant "again", not "the one before". */
    if (tp_player_position_ms(app->player) >= TP_PREV_RESTART_MS)
        return tp_app_play_current(app);

    if (step_to_playable(&app->queue, tp_queue_prev) != 0)
        return 1;
    return tp_app_play_current(app);
}

int tp_app_cmd_shuffle(struct tp_app *app)
{
    if (!app->loaded && tp_app_load(app) != 0)
        return 1;
    app->cfg.shuffle = !app->cfg.shuffle;
    tp_queue_from_library(&app->queue, &app->lib, app->cfg.shuffle);
    tp_config_save(&app->cfg);
    printf("shuffle: %s\n", app->cfg.shuffle ? "on" : "off");
    return 0;
}

static const char *state_name(enum tp_player_state st)
{
    switch (st) {
    case TP_PLAYER_PLAYING: return "playing";
    case TP_PLAYER_PAUSED: return "paused";
    default: return "stopped";
    }
}

int tp_app_cmd_status(struct tp_app *app)
{
    enum tp_player_state st = tp_player_state(app->player);
    unsigned long pos = tp_player_position_ms(app->player);
    unsigned long dur = tp_player_duration_ms(app->player);
    const char *err;

    printf("backend: %s\n", tp_player_backend_name(app->backend));
    printf("state: %s\n", state_name(st));
    if (st != TP_PLAYER_STOPPED) {
        const char *codec = tp_player_codec(app->player);
        printf("track: %s\n", tp_player_current_title(app->player));
        if (codec && codec[0])
            printf("format: %s %d Hz %d ch\n", codec, tp_player_rate(app->player),
                   tp_player_channels(app->player));
        printf("position: %lu:%02lu", pos / 60000ul, (pos / 1000ul) % 60ul);
        if (dur)
            printf(" / %lu:%02lu", dur / 60000ul, (dur / 1000ul) % 60ul);
        printf("\n");
    }
    printf("shuffle: %s\n", app->cfg.shuffle ? "on" : "off");
    if (app->cfg.last_track_id)
        printf("last_track_id: %llu\n", (unsigned long long)app->cfg.last_track_id);
    err = tp_player_last_error(app->player);
    if (err && err[0])
        printf("last_error: %s\n", err);
    return 0;
}

/*
 * Decode straight to a WAV file. This is how playback gets verified without a
 * speaker: same decoder the ALSA backend runs, output somewhere it can be
 * inspected, so a decode fault can be told apart from an audio-device fault.
 */
int tp_app_cmd_decode(struct tp_app *app, const char *path, const char *out)
{
    struct tp_dec *dec;
    struct tp_sink *sink;
    int16_t *block;
    char err[256] = "";
    unsigned long long total = 0;
    int n;

    (void)app;

    dec = tp_dec_open(path, err, sizeof(err));
    if (!dec) {
        tp_error("Cannot decode this file.\n  %s\n  %s", path, err);
        return 1;
    }
    block = malloc(sizeof(int16_t) * TP_DEC_MAX_BLOCK);
    if (!block) {
        tp_dec_close(dec);
        return 1;
    }
    sink = tp_sink_open_wav(out, tp_dec_rate(dec), tp_dec_channels(dec), err, sizeof(err));
    if (!sink) {
        tp_error("%s", err);
        free(block);
        tp_dec_close(dec);
        return 1;
    }

    while ((n = tp_dec_read(dec, block)) > 0) {
        if (tp_sink_write(sink, block, n) != 0) {
            tp_error("write to %s failed", out);
            break;
        }
        total += (unsigned long long)n;
    }

    printf("%s: %s %d Hz %d ch, %llu samples (%.2f s) -> %s\n", path,
           tp_dec_codec_name(dec), tp_dec_rate(dec), tp_dec_channels(dec), total,
           tp_dec_channels(dec) && tp_dec_rate(dec)
               ? (double)total / tp_dec_channels(dec) / tp_dec_rate(dec)
               : 0.0,
           out);

    tp_sink_close(sink);
    free(block);
    tp_dec_close(dec);
    return n < 0 ? 1 : 0;
}
