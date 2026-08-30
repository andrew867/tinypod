/*
 * preview.c — render every TinyPod screen headlessly, to PNG.
 *
 * No framebuffer, no device, no music. It drives the screen functions directly
 * with a made-up library, because the states worth looking at are the awkward
 * ones: a list longer than the screen, a title too long for its row, an album
 * with no artist, an empty library, a failed decode.
 *
 * Every layout bug in the other three N31 apps was found in a picture and none
 * of them were visible in the source.
 */

#include "tp_lv_screens.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static uint32_t s_fb[TP_LV_W * TP_LV_H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    (void)a; (void)p;
    lv_display_flush_ready(d);
}

static bool write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54];
    uint32_t data, off, total, ihdr;
    int32_t ww, hh;
    uint16_t planes, bpp;
    bool ok;
    int y;

    if (!f) return false;

    data = (uint32_t)(w * h * 4);
    off = 54;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    total = off + data;
    memcpy(hdr + 2, &total, 4);
    memcpy(hdr + 10, &off, 4);
    ihdr = 40;
    memcpy(hdr + 14, &ihdr, 4);
    ww = w; hh = h;
    memcpy(hdr + 18, &ww, 4);
    memcpy(hdr + 22, &hh, 4);
    planes = 1; bpp = 32;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data, 4);

    ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    for (y = h - 1; y >= 0 && ok; y--)
        ok = fwrite(px + (size_t)y * w, 4, (size_t)w, f) == (size_t)w;
    fclose(f);
    return ok;
}

static const char *s_out = "shots";
static int s_n;

static void shot(const char *name)
{
    char path[512];
    int i;

    for (i = 0; i < 8; i++) { lv_refr_now(NULL); lv_timer_handler(); }

    snprintf(path, sizeof path, "%s/%d-%s.bmp", s_out, ++s_n, name);
    printf("  %s\n", write_bmp(path, s_fb, TP_LV_W, TP_LV_H) ? path : "FAILED");
}

/* ---- a made-up library ---------------------------------------------------- */

struct song { const char *title, *artist, *album, *dur; };

static const struct song k_songs[] = {
    { "Windowlicker", "Aphex Twin", "Windowlicker", "6:07" },
    { "Xtal", "Aphex Twin", "Selected Ambient Works 85-92", "4:51" },
    { "A Very Long Song Title That Cannot Possibly Fit",
      "Some Artist With A Long Name", "An Album", "3:12" },
    { "Teardrop", "Massive Attack", "Mezzanine", "5:29" },
    { "Angel", "Massive Attack", "Mezzanine", "6:18" },
    { "Roads", "Portishead", "Dummy", "5:02" },
    { "Glory Box", "Portishead", "Dummy", "5:06" },
    { "Untitled", NULL, NULL, NULL },
};

#define N_SONGS ((int)(sizeof k_songs / sizeof k_songs[0]))

static void fill_songs(int i, struct tp_lv_row *out, void *ctx)
{
    (void)ctx;
    out->line1 = k_songs[i].title;
    out->line2 = k_songs[i].artist;
    out->badge = k_songs[i].dur;
    out->playing = (i == 3);
}

static const char *k_menu[] = {
    "Songs", "Artists", "Albums", "Playlists",
    "Shuffle All", "Now Playing", "Settings", "About"
};

static const char *k_menu_sub[] = {
    "412 tracks", "58", "61", "7", "Play the library at random", NULL, NULL, NULL
};

static void fill_menu(int i, struct tp_lv_row *out, void *ctx)
{
    (void)ctx;
    out->line1 = k_menu[i];
    out->line2 = k_menu_sub[i];
}

static void fill_artists(int i, struct tp_lv_row *out, void *ctx)
{
    static const char *n[] = { "Aphex Twin", "Boards of Canada", "Massive Attack",
                               "Portishead", "Radiohead", "Talk Talk", "Tortoise" };
    static const char *c[] = { "34", "52", "28", "19", "77", "23", "41" };
    (void)ctx;
    out->line1 = n[i];
    out->badge = c[i];
}

static void fill_settings(int i, struct tp_lv_row *out, void *ctx)
{
    static const char *n[] = { "Shuffle", "Audio backend", "Library source",
                               "Stop playback" };
    static const char *v[] = { "On", "alsa", "412 tracks",
                               "Ends the current track" };
    (void)ctx;
    out->line1 = n[i];
    out->line2 = v[i];
}

static void fill_empty(int i, struct tp_lv_row *out, void *ctx)
{
    (void)i; (void)out; (void)ctx;
}

int main(int argc, char **argv)
{
    lv_display_t *d;
    struct tp_lv_now n;

    if (argc > 1) s_out = argv[1];

    lv_init();
    d = lv_display_create(TP_LV_W, TP_LV_H);
    lv_display_set_buffers(d, s_fb, NULL, sizeof s_fb,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);

    tp_lv_screens_init();

    tp_lv_show_list("TinyPod", 8, 0, 0, fill_menu, NULL, NULL);
    tp_lv_set_hint("VOL move   PLAY open");
    shot("menu");

    tp_lv_show_list("Songs", N_SONGS, 3, 0, fill_songs, NULL, "No tracks found");
    tp_lv_set_hint("PLAY play   hold PLAY back");
    shot("songs");

    /* Scrolled, so the indicator is off the top and the long title is on
       screen where it can be seen to elide. */
    tp_lv_show_list("Songs", N_SONGS, 7, 2, fill_songs, NULL, NULL);
    shot("songs-scrolled");

    tp_lv_show_list("Artists", 7, 2, 0, fill_artists, NULL, "No artists");
    tp_lv_set_hint("PLAY open   hold PLAY back");
    shot("artists");

    tp_lv_show_list("Settings", 4, 0, 0, fill_settings, NULL, NULL);
    tp_lv_set_hint("PLAY change   hold PLAY back");
    shot("settings");

    tp_lv_show_list("Playlists", 0, 0, 0, fill_empty, NULL, "No playlists");
    tp_lv_set_hint("hold PLAY back");
    shot("empty");

    memset(&n, 0, sizeof n);
    n.title = "Teardrop";
    n.artist = "Massive Attack";
    n.album = "Mezzanine";
    n.codec = "AAC";
    n.pos_ms = 141000;
    n.dur_ms = 329000;
    n.state = 1;
    n.index = 4; n.total = 412;
    n.shuffle = true;
    tp_lv_show_now(&n);
    tp_lv_set_hint("VOL track   PLAY pause   hold back");
    shot("now");

    /* Paused, with a title too long for the width. */
    n.title = "A Very Long Song Title That Cannot Possibly Fit";
    n.artist = "Some Artist With A Long Name";
    n.state = 2;
    n.shuffle = false;
    tp_lv_show_now(&n);
    shot("now-long");

    /* Nothing playing, and a decode that failed - the case where the
       transport would be a lie. */
    memset(&n, 0, sizeof n);
    n.title = "Broken.m4a";
    n.error = "decoder said no";
    tp_lv_show_now(&n);
    shot("now-error");

    /* The scan screen, in the three states worth seeing: a stage that cannot
       report progress, one that can, and the counts it reports once the
       database has been read. */
    tp_lv_show_scan("/mnt/disk/iPod_Control", "reading the database", -1);
    tp_lv_set_hint("");
    shot("scan");

    tp_lv_show_scan("/mnt/disk/iPod_Control", "496 tracks, 117 artists", -1);
    shot("scan-counts");

    tp_lv_show_scan("/mnt/disk/iPod_Control", "checking music folders", 60);
    shot("scan-progress");

    tp_lv_show_message("About",
                       "TinyPod\n\n"
                       "Read-only iPod music\n"
                       "for N31 Linux.\n\n"
                       "Nothing under iPod_Control\n"
                       "is ever written.");
    tp_lv_set_hint("hold PLAY back");
    shot("about");

    /* The long-press indicator, which only ever appears mid-press. */
    tp_lv_show_list("Songs", N_SONGS, 3, 0, fill_songs, NULL, NULL);
    tp_lv_set_hint("PLAY play   hold PLAY back");
    tp_lv_set_holding(true);
    shot("holding");

    return 0;
}
