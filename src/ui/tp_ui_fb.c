#include "tp_app.h"
#include "tp_log.h"
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
};

static void draw_term(struct ui_state *ui)
{
    size_t i;
    printf("\033[2J\033[H");
    printf("==== TinyPod ====\n");
    switch (ui->screen) {
    case UI_HOME:
        printf("  %s Songs\n", ui->sel == 0 ? ">" : " ");
        printf("  %s Artists\n", ui->sel == 1 ? ">" : " ");
        printf("  %s Albums\n", ui->sel == 2 ? ">" : " ");
        printf("  %s Playlists\n", ui->sel == 3 ? ">" : " ");
        printf("  %s Shuffle Songs\n", ui->sel == 4 ? ">" : " ");
        printf("  %s Now Playing\n", ui->sel == 5 ? ">" : " ");
        printf("  %s Settings\n", ui->sel == 6 ? ">" : " ");
        printf("  %s About\n", ui->sel == 7 ? ">" : " ");
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
    case UI_NOW:
        printf("Now Playing\n");
        tp_app_cmd_status(ui->app);
        break;
    case UI_SETTINGS:
        printf("Settings\n  shuffle: %s\n  backend: %s\n",
               ui->app->cfg.shuffle ? "on" : "off",
               tp_player_backend_name(ui->app->backend));
        break;
    case UI_ABOUT:
        printf("TinyPod — free iPod-native player for N31 Linux\n"
               "Read-only. No sync. No database rebuild.\n");
        break;
    }
    fflush(stdout);
}

static void activate(struct ui_state *ui)
{
    if (ui->screen == UI_HOME) {
        switch (ui->sel) {
        case 0: ui->screen = UI_SONGS; ui->sel = 0; break;
        case 1: ui->screen = UI_ARTISTS; ui->sel = 0; break;
        case 2: ui->screen = UI_ALBUMS; ui->sel = 0; break;
        case 3: ui->screen = UI_PLAYLISTS; ui->sel = 0; break;
        case 4:
            ui->app->cfg.shuffle = 1;
            tp_queue_from_library(&ui->app->queue, &ui->app->lib, 1);
            if (ui->app->queue.count &&
                tp_app_cmd_play_id(ui->app, ui->app->queue.ids[0]) == 0)
                ui->was_playing = 1;
            ui->screen = UI_NOW;
            break;
        case 5: ui->screen = UI_NOW; break;
        case 6: ui->screen = UI_SETTINGS; break;
        case 7: ui->screen = UI_ABOUT; break;
        }
    } else if (ui->screen == UI_SONGS && ui->app->lib.track_count) {
        size_t idx = (size_t)ui->sel;
        if (idx < ui->app->lib.track_count &&
            tp_app_cmd_play_id(ui->app, ui->app->lib.tracks[idx].track_id) == 0)
            ui->was_playing = 1;
        ui->screen = UI_NOW;
    } else if (ui->screen == UI_PLAYLISTS && ui->app->lib.playlist_count) {
        size_t idx = (size_t)ui->sel;
        if (idx < ui->app->lib.playlist_count &&
            ui->app->lib.playlists[idx].track_count > 0 &&
            tp_app_cmd_play_id(ui->app, ui->app->lib.playlists[idx].track_ids[0]) == 0)
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
    default: return 0;
    }
}

int tp_ui_fb_run(struct tp_app *app)
{
    struct ui_state ui;
    struct termios oldt, newt;
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
        newt = oldt;
        newt.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }

    while (ui.running) {
        int c;
        int mx;
        enum tp_player_state st;
        fd_set rfds;
        struct timeval tv;
        unsigned char ch;

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
        if (read(STDIN_FILENO, &ch, 1) != 1)
            continue;
        c = ch;

        st = tp_player_state(app->player);
        if (st == TP_PLAYER_PLAYING)
            ui.was_playing = 1;

        if (c == 'q' || c == 27) {
            if (ui.screen == UI_HOME)
                ui.running = 0;
            else {
                ui.screen = UI_HOME;
                ui.sel = 0;
            }
            continue;
        }
        mx = max_sel(&ui);
        if (mx < 0)
            mx = 0;
        if (c == 'j' || c == 's' || c == '+' ) {
            if (ui.sel < mx)
                ui.sel++;
        } else if (c == 'k' || c == 'w' || c == '-') {
            if (ui.sel > 0)
                ui.sel--;
        } else if (c == '\n' || c == ' ') {
            activate(&ui);
        } else if (c == 'p') {
            if (tp_player_state(app->player) == TP_PLAYER_PLAYING)
                tp_app_cmd_pause(app);
            else
                tp_app_cmd_resume(app);
        } else if (c == 'n') {
            tp_app_cmd_next(app);
            ui.was_playing = 1;
        } else if (c == 'b') {
            tp_app_cmd_prev(app);
            ui.was_playing = 1;
        } else if (c == 'x') {
            tp_app_cmd_stop(app);
            ui.was_playing = 0;   /* a deliberate stop must not auto-advance */
        }
    }

    if (tcgetattr(STDIN_FILENO, &oldt) == 0)
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    printf("\n");
    return 0;
}
