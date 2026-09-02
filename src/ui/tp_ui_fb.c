#include "tp_app.h"
#include "util/tp_build.h"
#include "tp_log.h"
#include "tp_ui_keys.h"
#include "tp_util.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

/* Tiny framebuffer / terminal UI for N31 appliance mode.
 * Falls back to ANSI terminal menus when /dev/fb0 is unavailable (WSL/dev).
 */

enum ui_screen {
    UI_HOME = 0,
    UI_SONGS,
    UI_ARTISTS,
    UI_ALBUMS,
    UI_PLAYLISTS,
    UI_ARTIST_ALBUMS,  /* the albums by one artist */
    UI_TRACKS,         /* the tracks under one artist, album or playlist */
    UI_NOW,
    UI_SETTINGS,
    UI_ABOUT
};

struct ui_state {
    struct tp_app *app;
    enum ui_screen screen;
    int sel;
    int running;
    int was_playing;   /* to spot a track ending by itself */

    /*
     * The drill-down. Selecting an artist or an album used to do nothing at
     * all - activate() had no case for either - so those two lists were
     * signposts to rooms with no doors, and a playlist could only ever play
     * its first track.
     *
     * `filtered` holds indices into lib.tracks and `falbums` indices into
     * lib.albums, each built once on entry rather than filtered per row.
     *
     * There are three levels now - artist, that artist's albums, then the
     * tracks on one - so there is a real stack. Two ad-hoc back_to fields
     * covered one level and would not have survived this one.
     */
    size_t *filtered;
    size_t  filtered_n;
    size_t *falbums;
    size_t  falbums_n;
    char    title[160];
    char    artist[96];        /* whose albums are showing, for the header */

    struct { enum ui_screen screen; int sel; } stack[4];
    int     depth;
};

static void filtered_free(struct ui_state *ui)
{
    free(ui->filtered);
    ui->filtered = NULL;
    ui->filtered_n = 0;
}

static void falbums_free(struct ui_state *ui)
{
    free(ui->falbums);
    ui->falbums = NULL;
    ui->falbums_n = 0;
}

/* Remember where we are, then go somewhere. Silently refusing to descend past
   the stack rather than overflowing it: the menu is four deep by
   construction, and a fifth level would be a bug in this file, not input. */
static void push_to(struct ui_state *ui, enum ui_screen to)
{
    if (ui->depth < (int)(sizeof ui->stack / sizeof ui->stack[0])) {
        ui->stack[ui->depth].screen = ui->screen;
        ui->stack[ui->depth].sel = ui->sel;
        ui->depth++;
    }
    ui->screen = to;
    ui->sel = 0;
}

/* One level back, to exactly the row that was selected on the way in. */
static int pop_back(struct ui_state *ui)
{
    if (ui->depth <= 0)
        return 0;
    ui->depth--;
    ui->screen = ui->stack[ui->depth].screen;
    ui->sel = ui->stack[ui->depth].sel;
    return 1;
}

static int str_same(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

/* Tracks belonging to the selected artist, album or playlist. */
static void build_filtered(struct ui_state *ui, enum ui_screen from, int which)
{
    struct tp_app *app = ui->app;
    size_t i, n = 0;

    filtered_free(ui);

    if (from == UI_PLAYLISTS) {
        struct tp_playlist *pl = &app->lib.playlists[which];
        ui->filtered = calloc(pl->track_count ? pl->track_count : 1,
                              sizeof *ui->filtered);
        if (!ui->filtered)
            return;
        /* Playlist order, not library order - that is the point of one. */
        for (i = 0; i < pl->track_count; i++) {
            size_t j;
            for (j = 0; j < app->lib.track_count; j++) {
                if (app->lib.tracks[j].track_id == pl->track_ids[i]) {
                    ui->filtered[n++] = j;
                    break;
                }
            }
        }
        ui->filtered_n = n;
        snprintf(ui->title, sizeof ui->title, "%s",
                 pl->name ? pl->name : "?");
        return;
    }

    ui->filtered = calloc(app->lib.track_count ? app->lib.track_count : 1,
                          sizeof *ui->filtered);
    if (!ui->filtered)
        return;

    for (i = 0; i < app->lib.track_count; i++) {
        struct tp_track *t = &app->lib.tracks[i];
        if (from == UI_ARTISTS) {
            if (str_same(t->artist, app->lib.artists[which].name))
                ui->filtered[n++] = i;
        } else if (from == UI_ARTIST_ALBUMS) {
            /* This artist's tracks on this album. Matched against the artist
               we drilled in from, not against the album entry's own artist:
               the entry is one of possibly several for the same record, and
               filtering by its credit would hide the tracks credited the
               other way - which is the same split that made it two entries. */
            struct tp_album_entry *a = &app->lib.albums[ui->falbums[which]];
            if (str_same(t->album, a->album) &&
                (str_same(t->artist, ui->artist) ||
                 str_same(t->album_artist, ui->artist)))
                ui->filtered[n++] = i;
        } else {
            struct tp_album_entry *a = &app->lib.albums[which];
            /* album_artist as well as artist, or a compilation shows one
               track and hides the rest of its own album. */
            if (str_same(t->album, a->album) &&
                (str_same(t->artist, a->artist) ||
                 str_same(t->album_artist, a->artist)))
                ui->filtered[n++] = i;
        }
    }
    ui->filtered_n = n;

    if (from == UI_ARTISTS) {
        snprintf(ui->title, sizeof ui->title, "%s",
                 app->lib.artists[which].name);
    } else if (from == UI_ARTIST_ALBUMS) {
        struct tp_album_entry *a = &app->lib.albums[ui->falbums[which]];
        snprintf(ui->title, sizeof ui->title, "%s - %s", ui->artist, a->album);
    } else {
        struct tp_album_entry *a = &app->lib.albums[which];
        snprintf(ui->title, sizeof ui->title, "%s - %s", a->artist, a->album);
    }
}

/* The albums one artist appears on, as indices into lib.albums.
 *
 * Matched on the album entry's artist OR any of that artist's tracks naming
 * it, so a guest appearance on somebody else's record still shows up under
 * the guest - which is what a listener means by "their albums" and is not
 * what a strict match on the album's own artist field gives.
 */
static void build_artist_albums(struct ui_state *ui, int which)
{
    struct tp_app *app = ui->app;
    const char *who = app->lib.artists[which].name;
    size_t i, j, n = 0;

    falbums_free(ui);
    snprintf(ui->artist, sizeof ui->artist, "%s", who ? who : "?");

    ui->falbums = calloc(app->lib.album_count ? app->lib.album_count : 1,
                         sizeof *ui->falbums);
    if (!ui->falbums)
        return;

    for (i = 0; i < app->lib.album_count; i++) {
        struct tp_album_entry *a = &app->lib.albums[i];
        int mine = str_same(a->artist, who);
        size_t k;

        for (j = 0; !mine && j < app->lib.track_count; j++) {
            struct tp_track *t = &app->lib.tracks[j];
            if (str_same(t->artist, who) && str_same(t->album, a->album))
                mine = 1;
        }
        if (!mine)
            continue;

        /* One record can be two album entries: the library holds one per
           (artist, album) pair, so a title credited one way on some tracks
           and another way on others appears twice, identically, with nothing
           on screen to say why. Keep the first and drop the rest - the track
           list below matches on the artist we came from rather than on the
           entry's own artist, so nothing is lost with the duplicate row. */
        for (k = 0; k < n; k++)
            if (str_same(app->lib.albums[ui->falbums[k]].album, a->album))
                break;
        if (k == n)
            ui->falbums[n++] = i;
    }
    ui->falbums_n = n;
}

/* Enter the track list for row `which` of the list we are on. */
static void drill_in(struct ui_state *ui, enum ui_screen from, int which)
{
    build_filtered(ui, from, which);
    push_to(ui, UI_TRACKS);
}

static void draw_term(struct ui_state *ui)
{
    size_t i;
    printf("\033[2J\033[H");
    printf("==== TinyPod ====\n");
    /* At the top of every screen, because with no volume every list is
       empty, and an empty list looks like a library that failed to
       read rather than a disk that was never mounted. */
    if (ui->app->no_volume)
        printf("  no music volume - mount the disk, then restart\n");
    switch (ui->screen) {
    case UI_HOME:
        /* The same order as the graphical UI's k_menu. Two front ends over
           one library are only confusing when their menus disagree. */
        printf("  %s Shuffle All\n", ui->sel == 0 ? ">" : " ");
        printf("  %s Songs\n", ui->sel == 1 ? ">" : " ");
        printf("  %s Artists\n", ui->sel == 2 ? ">" : " ");
        printf("  %s Albums\n", ui->sel == 3 ? ">" : " ");
        printf("  %s Playlists\n", ui->sel == 4 ? ">" : " ");
        /* Folders is graphical-only: a drill-down browser over ssh is
           what the shell you are already in does better. The row is
           listed so the two menus agree and nothing is off by one. */
        printf("  %s Folders (graphical UI only)\n", ui->sel == 5 ? ">" : " ");
        printf("  %s Now Playing\n", ui->sel == 6 ? ">" : " ");
        printf("  %s Settings\n", ui->sel == 7 ? ">" : " ");
        printf("  %s About\n", ui->sel == 8 ? ">" : " ");
        printf("\nj/k move  Enter select  p pause  n next  b prev  x stop  q back\n");
        break;
    case UI_SONGS:
        printf("Songs (%zu)\n", ui->app->lib.track_count);
        for (i = 0; i < ui->app->lib.track_count && i < 20; i++) {
            struct tp_track *t = &ui->app->lib.tracks[i];
            printf("  %s %s - %s\n", (int)i == ui->sel ? ">" : " ",
                   t->artist ? t->artist : "?", t->title ? t->title : "?");
        }
        break;
    case UI_ARTISTS:
        printf("Artists (%zu)\n", ui->app->lib.artist_count);
        for (i = 0; i < ui->app->lib.artist_count && i < 20; i++)
            printf("  %s %s (%zu)\n", (int)i == ui->sel ? ">" : " ",
                   ui->app->lib.artists[i].name, ui->app->lib.artists[i].track_count);
        break;
    case UI_ALBUMS:
        printf("Albums (%zu)\n", ui->app->lib.album_count);
        for (i = 0; i < ui->app->lib.album_count && i < 20; i++)
            printf("  %s %s - %s\n", (int)i == ui->sel ? ">" : " ",
                   ui->app->lib.albums[i].artist, ui->app->lib.albums[i].album);
        break;
    case UI_PLAYLISTS:
        printf("Playlists (%zu)\n", ui->app->lib.playlist_count);
        for (i = 0; i < ui->app->lib.playlist_count && i < 20; i++)
            printf("  %s %s (%zu)\n", (int)i == ui->sel ? ">" : " ",
                   ui->app->lib.playlists[i].name, ui->app->lib.playlists[i].track_count);
        break;
    case UI_ARTIST_ALBUMS:
        printf("%s (%zu albums)\n", ui->artist, ui->falbums_n);
        if (!ui->falbums_n)
            printf("  (no albums)\n");
        for (i = 0; i < ui->falbums_n && i < 20; i++)
            printf("  %s %s\n", (int)i == ui->sel ? ">" : " ",
                   ui->app->lib.albums[ui->falbums[i]].album);
        break;
    case UI_TRACKS:
        printf("%s (%zu)\n", ui->title, ui->filtered_n);
        if (!ui->filtered_n)
            printf("  (no tracks)\n");
        for (i = 0; i < ui->filtered_n && i < 20; i++) {
            struct tp_track *t = &ui->app->lib.tracks[ui->filtered[i]];
            printf("  %s %s\n", (int)i == ui->sel ? ">" : " ",
                   t->title ? t->title : "?");
        }
        break;
    case UI_NOW:
        printf("Now Playing\n");
        tp_app_cmd_status(ui->app);
        /* The device is accepting samples far faster than it could
           play them, so nothing is coming out. Worth saying: the
           symptom used to be a clock that ran fast, and now that the
           clock is bounded there would be no symptom at all. */
        if (tp_player_not_pacing(ui->app->player))
            printf("  WARNING: the audio device is not playing what "
                   "it is given\n");
        break;
    case UI_SETTINGS:
        printf("Settings\n  shuffle: %s\n  backend: %s\n",
               ui->app->cfg.shuffle ? "on" : "off",
               tp_player_backend_name(ui->app->backend));
        break;
    case UI_ABOUT:
        printf("TinyPod\nbuild %s\n", tp_build_version());
        break;
    }
    fflush(stdout);
}

static void activate(struct ui_state *ui)
{
    if (ui->screen == UI_HOME) {
        switch (ui->sel) {
        case 0:
            ui->app->cfg.shuffle = 1;
            tp_queue_from_library(&ui->app->queue, &ui->app->lib, 1);
            if (ui->app->queue.count &&
                tp_app_cmd_play_id(ui->app, ui->app->queue.ids[0]) == 0)
                ui->was_playing = 1;
            ui->screen = UI_NOW;
            break;
        case 1: ui->screen = UI_SONGS; ui->sel = 0; break;
        case 2: ui->screen = UI_ARTISTS; ui->sel = 0; break;
        case 3: ui->screen = UI_ALBUMS; ui->sel = 0; break;
        case 4: ui->screen = UI_PLAYLISTS; ui->sel = 0; break;
        case 5: break;                 /* Folders: graphical UI only */
        case 6: ui->screen = UI_NOW; break;
        case 7: ui->screen = UI_SETTINGS; break;
        case 8: ui->screen = UI_ABOUT; break;
        }
    } else if (ui->screen == UI_SONGS && ui->app->lib.track_count) {
        size_t idx = (size_t)ui->sel;
        if (idx < ui->app->lib.track_count &&
            tp_app_cmd_play_id(ui->app, ui->app->lib.tracks[idx].track_id) == 0)
            ui->was_playing = 1;
        ui->screen = UI_NOW;
    } else if (ui->screen == UI_ARTISTS && ui->app->lib.artist_count) {
        if ((size_t)ui->sel < ui->app->lib.artist_count) {
            build_artist_albums(ui, ui->sel);
            /* One album, or none the library knows about: skip the list of
               one and go straight to the tracks. A menu whose only entry is
               the thing you already asked for is a keypress for nothing. */
            if (ui->falbums_n == 1) {
                build_filtered(ui, UI_ARTIST_ALBUMS, 0);
                push_to(ui, UI_TRACKS);
            } else if (ui->falbums_n == 0) {
                drill_in(ui, UI_ARTISTS, ui->sel);
            } else {
                push_to(ui, UI_ARTIST_ALBUMS);
            }
        }
    } else if (ui->screen == UI_ARTIST_ALBUMS && ui->falbums_n) {
        if ((size_t)ui->sel < ui->falbums_n) {
            build_filtered(ui, UI_ARTIST_ALBUMS, ui->sel);
            push_to(ui, UI_TRACKS);
        }
    } else if (ui->screen == UI_ALBUMS && ui->app->lib.album_count) {
        if ((size_t)ui->sel < ui->app->lib.album_count)
            drill_in(ui, UI_ALBUMS, ui->sel);
    } else if (ui->screen == UI_PLAYLISTS && ui->app->lib.playlist_count) {
        /* Into the playlist, not straight into its first track. Playing track
           one was the only thing a playlist could do, which left the rest of
           it unreachable. */
        if ((size_t)ui->sel < ui->app->lib.playlist_count)
            drill_in(ui, UI_PLAYLISTS, ui->sel);
    } else if (ui->screen == UI_TRACKS && ui->filtered_n) {
        size_t idx = (size_t)ui->sel;
        if (idx < ui->filtered_n &&
            tp_app_cmd_play_id(ui->app,
                ui->app->lib.tracks[ui->filtered[idx]].track_id) == 0)
            ui->was_playing = 1;
        ui->screen = UI_NOW;
    }
}

static int max_sel(struct ui_state *ui)
{
    switch (ui->screen) {
    case UI_HOME: return 7;
    case UI_SONGS: return (int)ui->app->lib.track_count - 1;
    case UI_ARTISTS: return (int)ui->app->lib.artist_count - 1;
    case UI_ALBUMS: return (int)ui->app->lib.album_count - 1;
    case UI_PLAYLISTS: return (int)ui->app->lib.playlist_count - 1;
    case UI_ARTIST_ALBUMS: return (int)ui->falbums_n - 1;
    case UI_TRACKS: return (int)ui->filtered_n - 1;
    default: return 0;
    }
}

int tp_ui_fb_run(struct tp_app *app)
{
    struct ui_state ui;
    struct termios oldt, newt;
    int have_termios = 0;
    int fb = open("/dev/fb0", O_RDWR);
    memset(&ui, 0, sizeof(ui));
    ui.app = app;
    ui.screen = UI_HOME;
    ui.running = 1;

    if (fb >= 0) {
        /* Map FB for future pixel UI; terminal still drives input for v0.3 */
        struct fb_var_screeninfo vinfo;
        if (ioctl(fb, FBIOGET_VSCREENINFO, &vinfo) == 0)
            tp_info("framebuffer %ux%u available", vinfo.xres, vinfo.yres);
        close(fb);
    } else {
        tp_info("No /dev/fb0 — using terminal UI");
    }

    if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
        have_termios = 1;
        newt = oldt;
        newt.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }

    while (ui.running) {
        enum tp_key k;
        int mx;
        enum tp_player_state st;
        fd_set rfds;
        struct timeval tv;

        draw_term(&ui);

        /*
         * Playback runs on its own thread, so the UI must not sit in a
         * blocking read: it has to keep the clock moving on Now Playing and
         * notice when a track ends so the next one can start.
         */
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) {
            st = tp_player_state(app->player);
            if (ui.was_playing && st == TP_PLAYER_STOPPED) {
                ui.was_playing = 0;
                /* Ended on its own - roll on to the next queued track. */
                if (app->queue.count > 0 && tp_app_cmd_next(app) == 0)
                    ui.was_playing = 1;
            } else if (st == TP_PLAYER_PLAYING) {
                ui.was_playing = 1;
            }
            continue;
        }
        k = tp_ui_read_key(STDIN_FILENO);
        if (k == TP_KEY_NONE)
            continue;

        st = tp_player_state(app->player);
        if (st == TP_PLAYER_PLAYING)
            ui.was_playing = 1;

        /* Back goes up ONE level, not straight home. From a track list that
           is the list you drilled in from, with the row you came from still
           selected - dropping to the home menu from three taps deep is how
           browsing a library becomes tedious. Back on the home screen leaves;
           quit leaves from wherever you are. */
        if (k == TP_KEY_QUIT || k == TP_KEY_BACK || k == TP_KEY_LEFT) {
            if (k == TP_KEY_QUIT || ui.screen == UI_HOME) {
                ui.running = 0;
            } else if (ui.screen == UI_TRACKS) {
                filtered_free(&ui);
                if (!pop_back(&ui)) { ui.screen = UI_HOME; ui.sel = 0; }
            } else if (ui.screen == UI_ARTIST_ALBUMS) {
                falbums_free(&ui);
                if (!pop_back(&ui)) { ui.screen = UI_HOME; ui.sel = 0; }
            } else {
                ui.depth = 0;
                ui.screen = UI_HOME;
                ui.sel = 0;
            }
            continue;
        }

        mx = max_sel(&ui);
        if (mx < 0)
            mx = 0;

        switch (k) {
        case TP_KEY_DOWN:
            if (ui.sel < mx)
                ui.sel++;
            break;
        case TP_KEY_UP:
            if (ui.sel > 0)
                ui.sel--;
            break;
        case TP_KEY_SELECT:
        case TP_KEY_RIGHT:
            activate(&ui);
            break;
        case TP_KEY_PLAYPAUSE:
            if (tp_player_state(app->player) == TP_PLAYER_PLAYING)
                tp_app_cmd_pause(app);
            else
                tp_app_cmd_resume(app);
            break;
        case TP_KEY_NEXT:
            tp_app_cmd_next(app);
            ui.was_playing = 1;
            break;
        case TP_KEY_PREV:
            tp_app_cmd_prev(app);
            ui.was_playing = 1;
            break;
        case TP_KEY_STOP:
            tp_app_cmd_stop(app);
            ui.was_playing = 0;   /* a deliberate stop must not auto-advance */
            break;
        default:
            break;
        }
    }

    /* Put back what was saved on the way in. Re-reading the settings here and
       writing those back restored the raw no-echo mode we had just been using,
       which leaves the shell with no echo and no line editing - looking for all
       the world like it has hung. */
    if (have_termios)
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    printf("\n");
    return 0;
}
