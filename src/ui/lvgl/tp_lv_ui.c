/*
 * tp_lv_ui.c — navigation, and the loop that drives it.
 *
 * A stack of views rather than a switch over screen names. Every list works the
 * same way - a title, a count, a row filler - so Songs, Artists, Albums,
 * Playlists and the drill-downs under them are one implementation with a
 * different row filler, and "back" is popping the stack rather than a table of
 * which screen returns to which.
 *
 * Nothing here owns music state. The library comes from tp_app and playback is
 * the same tp_app_cmd_* calls the CLI makes, so the two front ends can never
 * disagree about what is playing.
 */

#include "tp_lv_ui.h"
#include "tp_lv_screens.h"
#include "tp_lv_input.h"

#include "tp_app.h"
#include "util/tp_build.h"
#include "tp_log.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_MS 33

/* Now Playing has a clock on it, so it redraws on a timer rather than only on
   a keypress. Everything else is static until you press something. */
#define NOW_TICK_MS 500

/* A track that played for less than this did not play. Generous enough that a
   genuinely short file is not mistaken for a failure, short enough that three
   of them in a row is unmistakably a device problem rather than a library. */
#define TP_TOO_SHORT_MS   1500

/* How many of those in a row before the queue stops and says so. Three rather
   than one, because a single unplayable file among many is worth skipping -
   it is the run that means the device is gone. */
#define TP_FAIL_RUN_MAX   3

enum view_kind {
    V_MENU = 0,
    V_SONGS,
    V_ARTISTS,
    V_ALBUMS,
    V_PLAYLISTS,
    V_ARTIST_ALBUMS, /* the albums by one artist */
    V_LETTERS,       /* initials, to jump into a long list with */
    V_TRACKS,        /* the tracks under an artist, album or playlist */
    V_NOW,
    V_SETTINGS,
    V_ABOUT
};

struct view {
    enum view_kind kind;
    int sel;
    int top;
    int filter;              /* which artist/album/playlist we came from */
    const char *title;       /* for V_TRACKS, the thing we drilled into */
};

#define MAX_DEPTH 6

struct ui {
    struct tp_app *app;
    struct view stack[MAX_DEPTH];
    int depth;
    int running;
    int was_playing;

    /* Consecutive tracks that ended almost immediately. A broken sink makes
       every track do that, and counting them is how the UI tells a dead
       device from a queue of very short files. Reset by anything that plays. */
    int fail_run;

    /*
     * Track indices for the current drill-down. Built once on entry rather
     * than filtered per row: a row filler runs six times a frame and walking
     * the whole library each time would be quadratic for no reason.
     */
    size_t *filtered;
    size_t filtered_n;

    /* The albums by the artist currently drilled into, as indices into
       lib.albums, and whose they are - the name is what the track filter
       below matches on, not the album entry's own credit. */
    size_t *falbums;
    size_t  falbums_n;
    char    artist[96];

    /* The initials present in the list being jumped through, and the first
       row under each. Built once on entry: walking five hundred titles for
       every drawn row would be quadratic, and the row filler runs for every
       visible row on every redraw. */
    char    letters[27];
    int     letter_row[27];
    int     letters_n;
    enum view_kind letter_src;

    char title_buf[96];
};

static struct ui s_ui;

/* ---- the home menu -------------------------------------------------------- */

static const struct {
    const char *name;
    const char *sub;
} k_menu[] = {
    { "Songs",       "Everything, by title" },
    { "Artists",     NULL },
    { "Albums",      NULL },
    { "Playlists",   NULL },
    { "Shuffle All", "Play the library at random" },
    { "Now Playing", NULL },
    { "Settings",    NULL },
    { "About",       NULL },
};

#define MENU_N ((int)(sizeof k_menu / sizeof k_menu[0]))

/* ---- helpers -------------------------------------------------------------- */

static unsigned long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 1000UL + (unsigned long)(t.tv_nsec / 1000000);
}

/*
 * LVGL wants a uint32_t; now_ms returns unsigned long. Those are the same width
 * on this target and calling one through a pointer to the other is still
 * undefined behaviour, so convert properly rather than casting the pointer.
 */
static uint32_t lv_tick_ms(void)
{
    return (uint32_t)now_ms();
}

static struct view *top_view(void)
{
    return &s_ui.stack[s_ui.depth];
}

static const char *safe(const char *s, const char *alt)
{
    return (s && s[0]) ? s : alt;
}

static void dur_badge(uint32_t ms, char *out, size_t cap)
{
    unsigned long sec = ms / 1000;

    if (!ms) { out[0] = 0; return; }
    snprintf(out, cap, "%lu:%02lu", sec / 60, sec % 60);
}

/* Is this the track currently sounding? Compared by id, because the queue can
   hold the same track twice and the pointer would not tell them apart. */
static int is_current(struct tp_app *app, uint64_t id)
{
    if (tp_player_state(app->player) == TP_PLAYER_STOPPED)
        return 0;
    if (!app->queue.count || app->queue.pos >= app->queue.count)
        return 0;
    return app->queue.ids[app->queue.pos] == id;
}

/* ---- row fillers ---------------------------------------------------------- */

static void fill_menu(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    static char sub[8][40];

    out->line1 = k_menu[i].name;
    out->line2 = k_menu[i].sub;

    /* The counts are the useful part: "Artists 214" saves opening it to find
       out there are none. */
    switch (i) {
    case 0: snprintf(sub[0], sizeof sub[0], "%zu tracks", app->lib.track_count);
            out->line2 = sub[0]; break;
    case 1: snprintf(sub[1], sizeof sub[1], "%zu", app->lib.artist_count);
            out->line2 = sub[1]; break;
    case 2: snprintf(sub[2], sizeof sub[2], "%zu", app->lib.album_count);
            out->line2 = sub[2]; break;
    case 3: snprintf(sub[3], sizeof sub[3], "%zu", app->lib.playlist_count);
            out->line2 = sub[3]; break;
    default: break;
    }
}

/* Lists longer than this get a "jump to" row at the top. Below it the row
   would be in the way of the thing you were already looking at. */
#define JUMP_MIN 30

/* The initial a row sorts under: a letter, or '#' for everything else. */
static char initial_of(const char *name)
{
    char c;
    if (!name || !name[0]) return '#';
    c = name[0];
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return c;
    return '#';
}

static int raw_count(enum view_kind kind)
{
    struct tp_app *app = s_ui.app;
    switch (kind) {
    case V_SONGS:   return (int)app->lib.track_count;
    case V_ARTISTS: return (int)app->lib.artist_count;
    case V_ALBUMS:  return (int)app->lib.album_count;
    default:        return 0;
    }
}

/* The name row `i` of `kind` sorts under. */
static const char *row_name(enum view_kind kind, int i)
{
    struct tp_app *app = s_ui.app;
    switch (kind) {
    /* The tracks are sorted by artist and then album, so the Songs list is
       in ARTIST order and its jump has to index by artist. Indexing by title
       would land on one match with strangers either side. */
    case V_SONGS:   return app->lib.tracks[i].artist;
    case V_ARTISTS: return app->lib.artists[i].name;
    case V_ALBUMS:  return app->lib.albums[i].album;
    default:        return NULL;
    }
}

/* 1 when this view has a jump row above its entries, 0 otherwise. */
static int jump_offset(enum view_kind kind)
{
    switch (kind) {
    case V_SONGS:
    case V_ARTISTS:
    case V_ALBUMS:
        return raw_count(kind) >= JUMP_MIN ? 1 : 0;
    default:
        return 0;
    }
}

/* Collect the initials present and where each one starts. The lists are
   already in name order, so one pass finds every boundary. */
static void build_letters(enum view_kind kind)
{
    int n = raw_count(kind);
    char last = 0;
    int i;

    s_ui.letters_n = 0;
    s_ui.letter_src = kind;

    for (i = 0; i < n && s_ui.letters_n < 27; i++) {
        char c = initial_of(row_name(kind, i));
        if (c == last) continue;
        last = c;
        s_ui.letters[s_ui.letters_n] = c;
        s_ui.letter_row[s_ui.letters_n] = i;
        s_ui.letters_n++;
    }
}

/* The jump row itself, wherever a list is long enough to have one. */
static int fill_jump(int i, struct tp_lv_row *out, enum view_kind kind)
{
    if (!jump_offset(kind) || i != 0) return 0;
    out->line1 = "Jump to...";
    out->line2 = "by first letter";
    out->badge = NULL;
    return 1;
}

static void fill_songs(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    struct tp_track *t;
    static char badge[8][12];
    int slot = i % 8;

    if (fill_jump(i, out, V_SONGS)) return;
    i -= jump_offset(V_SONGS);
    t = &app->lib.tracks[i];

    out->line1 = safe(t->title, "Unknown title");
    out->line2 = safe(t->artist, "Unknown artist");
    dur_badge(t->duration_ms, badge[slot], sizeof badge[slot]);
    out->badge = badge[slot];
    out->playing = is_current(app, t->track_id) ? true : false;
}

static void fill_artists(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    static char badge[8][12];
    int slot = i % 8;

    if (fill_jump(i, out, V_ARTISTS)) return;
    i -= jump_offset(V_ARTISTS);

    out->line1 = safe(app->lib.artists[i].name, "Unknown artist");
    snprintf(badge[slot], sizeof badge[slot], "%zu",
             app->lib.artists[i].track_count);
    out->badge = badge[slot];
}

static void fill_albums(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    static char badge[8][12];
    int slot = i % 8;

    if (fill_jump(i, out, V_ALBUMS)) return;
    i -= jump_offset(V_ALBUMS);

    out->line1 = safe(app->lib.albums[i].album, "Unknown album");
    out->line2 = safe(app->lib.albums[i].artist, "Unknown artist");
    snprintf(badge[slot], sizeof badge[slot], "%zu",
             app->lib.albums[i].track_count);
    out->badge = badge[slot];
}

/* The initials, and how far into the list each one is. */
static void fill_letters(int i, struct tp_lv_row *out, void *ctx)
{
    static char name[8][4];
    static char badge[8][12];
    int slot = i % 8;
    (void)ctx;

    name[slot][0] = (i >= 0 && i < s_ui.letters_n) ? s_ui.letters[i] : '?';
    name[slot][1] = 0;
    out->line1 = name[slot];
    out->line2 = NULL;
    snprintf(badge[slot], sizeof badge[slot], "%d",
             (i >= 0 && i < s_ui.letters_n) ? s_ui.letter_row[i] + 1 : 0);
    out->badge = badge[slot];
}

static void fill_artist_albums(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    static char badge[8][12];
    int slot = i % 8;
    struct tp_album_entry *a = &app->lib.albums[s_ui.falbums[i]];

    out->line1 = safe(a->album, "Unknown album");
    out->line2 = s_ui.artist;
    snprintf(badge[slot], sizeof badge[slot], "%zu", a->track_count);
    out->badge = badge[slot];
}

static void fill_playlists(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    static char badge[8][12];
    int slot = i % 8;

    out->line1 = safe(app->lib.playlists[i].name, "Untitled playlist");
    snprintf(badge[slot], sizeof badge[slot], "%zu",
             app->lib.playlists[i].track_count);
    out->badge = badge[slot];
}

static void fill_tracks(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    struct tp_track *t = &app->lib.tracks[s_ui.filtered[i]];
    static char badge[8][12];
    int slot = i % 8;

    out->line1 = safe(t->title, "Unknown title");
    out->line2 = safe(t->album, NULL);
    dur_badge(t->duration_ms, badge[slot], sizeof badge[slot]);
    out->badge = badge[slot];
    out->playing = is_current(app, t->track_id) ? true : false;
}

static void fill_settings(int i, struct tp_lv_row *out, void *ctx)
{
    struct tp_app *app = ctx;
    static char v[4][48];

    switch (i) {
    case 0:
        out->line1 = "Shuffle";
        snprintf(v[0], sizeof v[0], "%s", app->cfg.shuffle ? "On" : "Off");
        out->line2 = v[0];
        break;
    case 1:
        out->line1 = "Audio backend";
        snprintf(v[1], sizeof v[1], "%s",
                 tp_player_backend_name(app->backend));
        out->line2 = v[1];
        break;
    case 2:
        out->line1 = "Library source";
        snprintf(v[2], sizeof v[2], "%zu tracks", app->lib.track_count);
        out->line2 = v[2];
        break;
    default:
        out->line1 = "Stop playback";
        out->line2 = "Ends the current track";
        break;
    }
}

#define SETTINGS_N 4

/* ---- building a drill-down ------------------------------------------------ */

static void filtered_free(void)
{
    free(s_ui.filtered);
    s_ui.filtered = NULL;
    s_ui.filtered_n = 0;
}

static void falbums_free(void)
{
    free(s_ui.falbums);
    s_ui.falbums = NULL;
    s_ui.falbums_n = 0;
}

static int str_eq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/*
 * Collect the tracks belonging to one artist, album or playlist. Done once on
 * entry: the row filler is called for every visible row on every redraw, and
 * scanning the library there would be quadratic in the library size.
 */
static void build_filtered(enum view_kind from, int which)
{
    struct tp_app *app = s_ui.app;
    size_t i, n = 0;

    filtered_free();

    if (from == V_PLAYLISTS) {
        struct tp_playlist *p = &app->lib.playlists[which];
        s_ui.filtered = calloc(p->track_count ? p->track_count : 1,
                               sizeof *s_ui.filtered);
        if (!s_ui.filtered) return;

        for (i = 0; i < p->track_count; i++) {
            size_t j;
            for (j = 0; j < app->lib.track_count; j++) {
                if (app->lib.tracks[j].track_id == p->track_ids[i]) {
                    s_ui.filtered[n++] = j;
                    break;
                }
            }
        }
        s_ui.filtered_n = n;
        return;
    }

    s_ui.filtered = calloc(app->lib.track_count ? app->lib.track_count : 1,
                           sizeof *s_ui.filtered);
    if (!s_ui.filtered) return;

    for (i = 0; i < app->lib.track_count; i++) {
        struct tp_track *t = &app->lib.tracks[i];

        if (from == V_ARTISTS) {
            if (str_eq(t->artist, app->lib.artists[which].name))
                s_ui.filtered[n++] = i;
        } else if (from == V_ARTIST_ALBUMS) {
            /* This artist's tracks on this album. Matched against the artist
               we drilled in from, not the album entry's own credit: the entry
               is one of possibly several for the same record, and filtering
               by its credit would hide the tracks credited the other way. */
            struct tp_album_entry *a = &app->lib.albums[s_ui.falbums[which]];
            if (str_eq(t->album, a->album) &&
                (str_eq(t->artist, s_ui.artist) ||
                 str_eq(t->album_artist, s_ui.artist)))
                s_ui.filtered[n++] = i;
        } else {
            struct tp_album_entry *a = &app->lib.albums[which];
            if (str_eq(t->album, a->album) &&
                (str_eq(t->artist, a->artist) ||
                 str_eq(t->album_artist, a->artist)))
                s_ui.filtered[n++] = i;
        }
    }
    s_ui.filtered_n = n;
}

/*
 * The albums one artist appears on, as indices into lib.albums.
 *
 * Matched on the album entry's artist OR any of that artist's tracks naming
 * it, so a guest appearance on somebody else's record still shows up under
 * the guest - which is what a listener means by "their albums" and is not
 * what a strict match on the entry's own artist field gives.
 */
static void build_artist_albums(int which)
{
    struct tp_app *app = s_ui.app;
    const char *who = app->lib.artists[which].name;
    size_t i, j, k, n = 0;

    falbums_free();
    snprintf(s_ui.artist, sizeof s_ui.artist, "%s", who ? who : "?");

    s_ui.falbums = calloc(app->lib.album_count ? app->lib.album_count : 1,
                          sizeof *s_ui.falbums);
    if (!s_ui.falbums) return;

    for (i = 0; i < app->lib.album_count; i++) {
        struct tp_album_entry *a = &app->lib.albums[i];
        int mine = str_eq(a->artist, who);

        for (j = 0; !mine && j < app->lib.track_count; j++) {
            struct tp_track *t = &app->lib.tracks[j];
            if (str_eq(t->artist, who) && str_eq(t->album, a->album))
                mine = 1;
        }
        if (!mine) continue;

        /* One record can be two album entries - the library holds one per
           (artist, album) pair - and they render identically, with nothing on
           screen to say why there are two. */
        for (k = 0; k < n; k++)
            if (str_eq(app->lib.albums[s_ui.falbums[k]].album, a->album))
                break;
        if (k == n) s_ui.falbums[n++] = i;
    }
    s_ui.falbums_n = n;
}

/* Defined below with the other drawing; declared here because opening a
   track redraws before it starts playback. */
static void draw(void);

/* ---- navigation ----------------------------------------------------------- */

static void push(enum view_kind kind, int filter, const char *title)
{
    struct view *v;

    if (s_ui.depth + 1 >= MAX_DEPTH)
        return;

    s_ui.depth++;
    v = top_view();
    memset(v, 0, sizeof *v);
    v->kind = kind;
    v->filter = filter;
    v->title = title;
}

static void pop(void)
{
    if (s_ui.depth > 0)
        s_ui.depth--;
    /* Each list belongs to the view that was popped. */
    if (top_view()->kind != V_TRACKS)
        filtered_free();
    if (top_view()->kind != V_ARTIST_ALBUMS)
        falbums_free();
}

static int view_count(struct view *v)
{
    struct tp_app *app = s_ui.app;

    switch (v->kind) {
    case V_MENU:      return MENU_N;
    case V_SONGS:     return (int)app->lib.track_count + jump_offset(V_SONGS);
    case V_ARTISTS:   return (int)app->lib.artist_count + jump_offset(V_ARTISTS);
    case V_ALBUMS:    return (int)app->lib.album_count + jump_offset(V_ALBUMS);
    case V_LETTERS:   return s_ui.letters_n;
    case V_PLAYLISTS: return (int)app->lib.playlist_count;
    case V_ARTIST_ALBUMS: return (int)s_ui.falbums_n;
    case V_TRACKS:    return (int)s_ui.filtered_n;
    case V_SETTINGS:  return SETTINGS_N;
    default:          return 0;
    }
}

static void play_index(size_t track_index)
{
    struct tp_app *app = s_ui.app;

    if (track_index >= app->lib.track_count)
        return;

    /*
     * Move to Now Playing and draw it BEFORE asking for playback. Starting a
     * track writes the config, reads the file header off NAND and joins the
     * previous decode thread, and none of that is instant on this device - so
     * it happens behind a screen that has already changed rather than a list
     * that has stopped responding.
     */
    push(V_NOW, 0, NULL);
    draw();
    lv_refr_now(NULL);

    if (tp_app_cmd_play_id(app, app->lib.tracks[track_index].track_id) == 0)
        s_ui.was_playing = 1;
}

static void activate(void)
{
    struct view *v = top_view();
    struct tp_app *app = s_ui.app;

    switch (v->kind) {
    case V_MENU:
        switch (v->sel) {
        case 0: push(V_SONGS, 0, NULL); break;
        case 1: push(V_ARTISTS, 0, NULL); break;
        case 2: push(V_ALBUMS, 0, NULL); break;
        case 3: push(V_PLAYLISTS, 0, NULL); break;
        case 4:
            app->cfg.shuffle = 1;
            tp_queue_from_library(&app->queue, &app->lib, 1);
            if (app->queue.count &&
                tp_app_cmd_play_id(app, app->queue.ids[0]) == 0)
                s_ui.was_playing = 1;
            push(V_NOW, 0, NULL);
            break;
        case 5: push(V_NOW, 0, NULL); break;
        case 6: push(V_SETTINGS, 0, NULL); break;
        default: push(V_ABOUT, 0, NULL); break;
        }
        break;

    case V_SONGS:
        if (jump_offset(V_SONGS) && v->sel == 0) {
            build_letters(V_SONGS);
            push(V_LETTERS, 0, "Songs by artist");
            break;
        }
        play_index((size_t)(v->sel - jump_offset(V_SONGS)));
        break;

    case V_LETTERS: {
        /* Land on the first entry under this initial, in the list we came
           from - so back from here is that list, already scrolled there. */
        int at = (v->sel >= 0 && v->sel < s_ui.letters_n)
                     ? s_ui.letter_row[v->sel] : 0;
        pop();
        top_view()->sel = at + jump_offset(s_ui.letter_src);
        break;
    }

    case V_ARTISTS:
        if (jump_offset(V_ARTISTS) && v->sel == 0) {
            build_letters(V_ARTISTS);
            push(V_LETTERS, 0, "Artists");
            break;
        }
        v->sel -= jump_offset(V_ARTISTS);
        build_artist_albums(v->sel);
        /* One album, or none the library knows about: skip the list of one.
           A menu whose only entry is the thing you already asked for is a
           keypress for nothing. */
        if (s_ui.falbums_n == 1) {
            build_filtered(V_ARTIST_ALBUMS, 0);
            push(V_TRACKS, 0, app->lib.artists[v->sel].name);
        } else if (s_ui.falbums_n == 0) {
            build_filtered(V_ARTISTS, v->sel);
            push(V_TRACKS, v->sel, app->lib.artists[v->sel].name);
        } else {
            push(V_ARTIST_ALBUMS, v->sel, app->lib.artists[v->sel].name);
        }
        break;

    case V_ARTIST_ALBUMS:
        if ((size_t)v->sel < s_ui.falbums_n) {
            build_filtered(V_ARTIST_ALBUMS, v->sel);
            push(V_TRACKS, v->sel,
                 app->lib.albums[s_ui.falbums[v->sel]].album);
        }
        break;

    case V_ALBUMS:
        if (jump_offset(V_ALBUMS) && v->sel == 0) {
            build_letters(V_ALBUMS);
            push(V_LETTERS, 0, "Albums");
            break;
        }
        v->sel -= jump_offset(V_ALBUMS);
        build_filtered(V_ALBUMS, v->sel);
        push(V_TRACKS, v->sel, app->lib.albums[v->sel].album);
        break;

    case V_PLAYLISTS:
        build_filtered(V_PLAYLISTS, v->sel);
        push(V_TRACKS, v->sel, app->lib.playlists[v->sel].name);
        break;

    case V_TRACKS:
        if ((size_t)v->sel < s_ui.filtered_n)
            play_index(s_ui.filtered[v->sel]);
        break;

    case V_NOW:
        if (tp_player_state(app->player) == TP_PLAYER_PLAYING) {
            tp_app_cmd_pause(app);
        } else if (tp_player_state(app->player) == TP_PLAYER_PAUSED) {
            tp_app_cmd_resume(app);
        } else if (app->queue.count) {
            tp_app_cmd_play_id(app, app->queue.ids[app->queue.pos]);
            s_ui.was_playing = 1;
        }
        break;

    case V_SETTINGS:
        if (v->sel == 0) {
            app->cfg.shuffle = !app->cfg.shuffle;
        } else if (v->sel == SETTINGS_N - 1) {
            tp_app_cmd_stop(app);
            s_ui.was_playing = 0;
        }
        break;

    default:
        break;
    }
}

/* ---- drawing -------------------------------------------------------------- */

static void draw(void)
{
    struct view *v = top_view();
    struct tp_app *app = s_ui.app;
    int count = view_count(v);
    int rows = tp_lv_list_rows();

    /* Clamp before drawing: the library can be reloaded under a view, and a
       selection past the end would index off the array. */
    if (v->sel >= count) v->sel = count > 0 ? count - 1 : 0;
    if (v->sel < 0) v->sel = 0;
    if (v->sel < v->top) v->top = v->sel;
    if (v->sel >= v->top + rows) v->top = v->sel - rows + 1;
    if (v->top > count - rows) v->top = count - rows;
    if (v->top < 0) v->top = 0;

    switch (v->kind) {
    case V_MENU:
        tp_lv_show_list("TinyPod", count, v->sel, v->top, fill_menu, app, NULL);
        tp_lv_set_hint("VOL move   PLAY open");
        break;

    case V_SONGS:
        tp_lv_show_list("Songs", count, v->sel, v->top, fill_songs, app,
                        "No tracks found");
        tp_lv_set_hint("PLAY play   hold PLAY back");
        break;

    case V_ARTISTS:
        tp_lv_show_list("Artists", count, v->sel, v->top, fill_artists, app,
                        "No artists");
        tp_lv_set_hint("PLAY open   hold PLAY back");
        break;

    case V_ALBUMS:
        tp_lv_show_list("Albums", count, v->sel, v->top, fill_albums, app,
                        "No albums");
        tp_lv_set_hint("PLAY open   hold PLAY back");
        break;

    case V_PLAYLISTS:
        tp_lv_show_list("Playlists", count, v->sel, v->top, fill_playlists,
                        app, "No playlists");
        tp_lv_set_hint("PLAY open   hold PLAY back");
        break;

    case V_LETTERS:
        snprintf(s_ui.title_buf, sizeof s_ui.title_buf, "%s",
                 safe(v->title, "Jump to"));
        tp_lv_show_list(s_ui.title_buf, count, v->sel, v->top, fill_letters,
                        app, "Nothing to jump to");
        tp_lv_set_hint("PLAY jump   hold PLAY back");
        break;

    case V_ARTIST_ALBUMS:
        snprintf(s_ui.title_buf, sizeof s_ui.title_buf, "%s",
                 safe(v->title, "Albums"));
        tp_lv_show_list(s_ui.title_buf, count, v->sel, v->top,
                        fill_artist_albums, app, "No albums");
        tp_lv_set_hint("PLAY open   hold PLAY back");
        break;

    case V_TRACKS:
        snprintf(s_ui.title_buf, sizeof s_ui.title_buf, "%s",
                 safe(v->title, "Tracks"));
        tp_lv_show_list(s_ui.title_buf, count, v->sel, v->top, fill_tracks,
                        app, "No tracks here");
        tp_lv_set_hint("PLAY play   hold PLAY back");
        break;

    case V_SETTINGS:
        tp_lv_show_list("Settings", count, v->sel, v->top, fill_settings, app,
                        NULL);
        tp_lv_set_hint("PLAY change   hold PLAY back");
        break;

    case V_NOW: {
        struct tp_lv_now n;
        struct tp_track *t = NULL;

        memset(&n, 0, sizeof n);

        if (app->queue.count && app->queue.pos < app->queue.count) {
            size_t i;
            uint64_t id = app->queue.ids[app->queue.pos];
            for (i = 0; i < app->lib.track_count; i++) {
                if (app->lib.tracks[i].track_id == id) {
                    t = &app->lib.tracks[i];
                    break;
                }
            }
            n.index = (int)app->queue.pos + 1;
            n.total = (int)app->queue.count;
        }

        if (t) {
            n.title = safe(t->title, "Unknown title");
            n.artist = safe(t->artist, "Unknown artist");
            n.album = safe(t->album, NULL);
        } else {
            n.title = tp_player_current_title(app->player);
        }

        n.pos_ms = tp_player_position_ms(app->player);
        n.dur_ms = tp_player_duration_ms(app->player);
        if (!n.dur_ms && t)
            n.dur_ms = t->duration_ms;

        n.codec = tp_player_codec(app->player);
        n.shuffle = app->cfg.shuffle ? true : false;

        switch (tp_player_state(app->player)) {
        case TP_PLAYER_PLAYING: n.state = 1; break;
        case TP_PLAYER_PAUSED:  n.state = 2; break;
        default:                n.state = 0; break;
        }

        {
            const char *e = tp_player_last_error(app->player);
            if (e && e[0] && n.state == 0)
                n.error = e;
            /* Said while it is still "playing", because that is when it is
               wrong and when the user is looking at it. The device is taking
               samples far faster than it could play them, so nothing is
               coming out - and without this the only clue was a clock that
               used to run fast and no longer does. */
            else if (tp_player_not_pacing(app->player))
                n.error = "audio device is not playing what it is given";
        }

        tp_lv_show_now(&n);
        tp_lv_set_hint("VOL track   PLAY pause   hold back");
        break;
    }

    default: {
        /* The build, on the one screen that exists to say what this is. An
           old copy on the device is indistinguishable from a fresh one
           without it. */
        static char about[256];
        snprintf(about, sizeof about,
                 "TinyPod\n\n"
                 "Read-only iPod music\n"
                 "for N31 Linux.\n\n"
                 "Nothing under iPod_Control\n"
                 "is ever written.\n\n"
                 "%s", tp_build_version());
        tp_lv_show_message("About", about);
        tp_lv_set_hint("hold PLAY back");
        break;
    }
    }
}

/* ---- keys ----------------------------------------------------------------- */

static void on_key(enum tp_lv_key k)
{
    struct view *v = top_view();
    struct tp_app *app = s_ui.app;

    /* On Now Playing the volume keys are transport, because there is no list
       to move through and skipping tracks is what you actually want there. */
    if (v->kind == V_NOW) {
        if (k == TP_LV_UP) {
            tp_app_cmd_prev(app);
            s_ui.was_playing = 1;
            return;
        }
        if (k == TP_LV_DOWN) {
            tp_app_cmd_next(app);
            s_ui.was_playing = 1;
            return;
        }
    }

    switch (k) {
    case TP_LV_UP:
        if (v->sel > 0) v->sel--;
        break;
    case TP_LV_DOWN:
        if (v->sel < view_count(v) - 1) v->sel++;
        break;
    case TP_LV_SELECT:
        activate();
        break;
    case TP_LV_BACK:
        /* At the top there is nowhere further back. HOME leaves the app, and
           that is the launcher's job, not ours. */
        if (s_ui.depth > 0) pop();
        break;
    case TP_LV_QUIT:
        s_ui.running = 0;
        break;
    default:
        break;
    }
}

/* Where tp_app_load reports to while the library is being read. */
static void scan_progress(const char *stage, int pct)
{
    struct tp_app *app = s_ui.app;
    tp_lv_show_scan(app ? app->vol.ipod_control_root : "", stage, pct);
}

/* ---- the loop ------------------------------------------------------------- */

/* ---- headless, for screenshots -------------------------------------------
 *
 * The same draw() and on_key() the device runs, over a display that writes to
 * memory. The point is that the screenshots come from the real view logic
 * with real library data rather than from a preview that fills the rows in
 * itself - the index arithmetic behind the jump row is exactly the sort of
 * thing that only a real list of five hundred titles will catch.
 */

static uint32_t s_shot_fb[TP_LV_W * TP_LV_H];

/* Nothing to copy: the display renders straight into s_shot_fb, because that
   is the buffer it was given. Copying out of the area pointer instead was the
   first version, and in DIRECT mode that pointer is into the whole-screen
   buffer rather than a packed rectangle - so it read the wrong pixels and
   produced a picture of rows sliding over each other. */
static void shot_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static int shot_write(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    int w = TP_LV_W, h = TP_LV_H;
    unsigned row = (unsigned)w * 3u, pad = (4u - row % 4u) % 4u;
    unsigned data = (row + pad) * (unsigned)h, size = 54u + data;
    unsigned char hdr[54] = { 0 };

    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)size; hdr[3] = (unsigned char)(size >> 8);
    hdr[4] = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
    hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t c = s_shot_fb[y * w + x];
            unsigned char b[3] = { (unsigned char)(c & 0xFF),
                                   (unsigned char)((c >> 8) & 0xFF),
                                   (unsigned char)((c >> 16) & 0xFF) };
            fwrite(b, 1, 3, f);
        }
        for (unsigned i = 0; i < pad; i++) fputc(0, f);
    }
    fclose(f);
    return 1;
}

int tp_lv_ui_shots(struct tp_app *app, const char *dir)
{
    lv_display_t *disp;
    char path[512];
    int made = 0;

    memset(&s_ui, 0, sizeof s_ui);
    s_ui.app = app;
    s_ui.running = 1;
    s_ui.stack[0].kind = V_MENU;

    lv_init();
    lv_tick_set_cb(lv_tick_ms);

    disp = lv_display_create(TP_LV_W, TP_LV_H);
    if (!disp) return 1;
    lv_display_set_buffers(disp, s_shot_fb, NULL, sizeof s_shot_fb,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, shot_flush);

    tp_lv_screens_init();

    if (!app->loaded && tp_app_load(app) != 0) {
        tp_error("shots: could not read the library");
        return 1;
    }

    /* A walk through the parts worth looking at, by pressing the buttons a
       person would press. */
    static const struct { const char *name; const char *keys; } WALK[] = {
        { "10-real-menu",         ""        },
        { "11-real-songs",        "ds"      },   /* down to Songs, select */
        { "12-real-jump",         "dss"     },   /* ...and into Jump to... */
        { "13-real-jumped",       "dssdddds" },  /* pick a letter */
        { "14-real-artists",      "dds"     },
        { "15-real-artist-albums", "ddsdddddddddddddds" },
    };

    for (unsigned i = 0; i < sizeof WALK / sizeof WALK[0]; i++) {
        /* Back to the top for each walk, so one sequence cannot leave the
           next somewhere it did not expect. */
        while (s_ui.depth > 0) pop();
        s_ui.stack[0].sel = 0;
        s_ui.stack[0].top = 0;

        for (const char *k = WALK[i].keys; *k; k++)
            on_key(*k == 'd' ? TP_LV_DOWN : *k == 'u' ? TP_LV_UP
                                                      : TP_LV_SELECT);

        draw();
        lv_refr_now(NULL);

        snprintf(path, sizeof path, "%s/%s.bmp", dir, WALK[i].name);
        if (shot_write(path)) { printf("  %s\n", path); made++; }
    }
    return made ? 0 : 1;
}

int tp_lv_ui_run(struct tp_app *app, const char *fb)
{
    lv_display_t *disp;
    unsigned long last_now = 0;

    memset(&s_ui, 0, sizeof s_ui);
    s_ui.app = app;
    s_ui.running = 1;
    s_ui.stack[0].kind = V_MENU;

    lv_init();
    lv_tick_set_cb(lv_tick_ms);

    disp = lv_linux_fbdev_create();
    if (!disp) {
        tp_error("no framebuffer - is /dev/fb0 there?");
        return 1;
    }
    lv_linux_fbdev_set_file(disp, fb ? fb : "/dev/fb0");
    lv_display_set_resolution(disp, TP_LV_W, TP_LV_H);

    if (tp_lv_input_open() == 0)
        tp_info("no button devices - nothing can be driven");

    /* This front end is still running when a track fails, so it does not need
       playback to block while it finds out. */
    tp_player_set_async(app->player, 1);

    tp_lv_screens_init();

    /*
     * Load the library HERE, behind a screen, rather than in main() before
     * this display existed. Five hundred tracks off a read-only vfat mount is
     * not instant, and until this moved the panel simply kept whatever the
     * launcher had left on it for the whole scan - which is indistinguishable
     * from an app that failed to start.
     */
    /* No volume at all: say so and stay up. Exiting here is what made this
       look like a crash from the launcher, and the explanation went to a tty
       nobody was looking at. */
    if (app->no_volume) {
        tp_lv_show_message("No music volume",
                           "Nothing is mounted.\n\n"
                           "Mount the disk with\n"
                           "n31-mount-disk, then\n"
                           "start TinyPod again.");
        tp_lv_set_hint("hold PLAY back");
        lv_refr_now(NULL);
    } else if (!app->loaded) {
        tp_lv_show_scan(app->vol.ipod_control_root, "starting", -1);
        tp_app_set_load_progress(app, scan_progress);
        int rc = tp_app_load(app);
        tp_app_set_load_progress(app, NULL);

        if (rc != 0) {
            tp_lv_show_message("No library",
                               "Could not read the iPod database.\n\n"
                               "Is the volume mounted?");
            lv_refr_now(NULL);
        }
    }

    draw();

    while (s_ui.running) {
        enum tp_lv_key k;
        int redraw = 0;

        while ((k = tp_lv_input_poll()) != TP_LV_NONE) {
            on_key(k);
            redraw = 1;
        }

        tp_lv_set_holding(tp_lv_input_holding() ? true : false);

        /*
         * Playback runs on its own thread, so a track ending has to be noticed
         * here - otherwise the queue stops at the end of whatever was started
         * by hand.
         */
        if (s_ui.was_playing &&
            tp_player_state(app->player) == TP_PLAYER_STOPPED) {
            const char *e = tp_player_last_error(app->player);

            s_ui.was_playing = 0;

            /*
             * A track that FAILED is not a track that finished, and which one
             * it was decides whether to move on.
             *
             * This used to advance on stopped alone. With a working device
             * those are the same thing; with a broken one they are not. The
             * sink fails to open, the track stops instantly, the queue steps
             * to the next, and the app walks the whole library in a few
             * seconds saying nothing - which is exactly what was reported.
             *
             * So an error stops the queue and stays on screen. And a run of
             * tracks that each end immediately is a broken device rather than
             * a run of empty files, so that stops too: a sink that fails
             * without setting a message would otherwise gallop just as
             * silently.
             */
            if (e && e[0]) {
                s_ui.fail_run = 0;
                redraw = 1;              /* Now Playing shows it; do not advance */
            } else {
                if (tp_player_position_ms(app->player) < TP_TOO_SHORT_MS)
                    s_ui.fail_run++;
                else
                    s_ui.fail_run = 0;

                if (s_ui.fail_run >= TP_FAIL_RUN_MAX) {
                    tp_lv_show_message(
                        "Playback stopped",
                        "Several tracks ended immediately.\n\n"
                        "The audio device is most likely refusing output. "
                        "The files themselves are probably fine.");
                } else if (app->queue.count > 0 &&
                           tp_app_cmd_next(app) == 0) {
                    s_ui.was_playing = 1;
                }
                redraw = 1;
            }
        } else if (tp_player_state(app->player) == TP_PLAYER_PLAYING) {
            s_ui.was_playing = 1;
            s_ui.fail_run = 0;
        }

        /* Now Playing has a clock on it and must tick without a keypress. */
        if (top_view()->kind == V_NOW &&
            now_ms() - last_now >= NOW_TICK_MS) {
            last_now = now_ms();
            redraw = 1;
        }

        if (redraw)
            draw();

        lv_timer_handler();
        usleep(FRAME_MS * 1000u);
    }

    filtered_free();
    tp_lv_input_close();
    return 0;
}
