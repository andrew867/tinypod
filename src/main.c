#include "tp_app.h"
#include "util/tp_build.h"
#ifdef TP_WITH_LVGL
#include "tp_lv_ui.h"
#endif
#include "tp_log.h"
#include "tp_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    /* In the banner, on the same stream. On stdout while the banner
       went to stderr it came out in the wrong order as soon as the
       output was piped: stdout is block-buffered when it is not a
       terminal and stderr is not, so the banner overtook it. */
    fprintf(stderr,
            "TinyPod — read-only iPod music player\n"
            "build %s\n\n", tp_build_version());
    fprintf(stderr,
            "Usage:\n"
            "  tinypod [--mount PATH] [--backend null|external|alsa] [--debug] <cmd> [args]\n\n"
            "Commands:\n"
            "  scan              Detect volume and load library\n"
            "  list              List tracks\n"
            "  search <query>    Search title/artist/album\n"
            "  libcheck [--json] Library health report\n"
            "  export-json       Normalized track JSON on stdout\n"
            "  export-m3u        M3U playlist on stdout\n"
            "  play <track-id>   Play by track id\n"
            "  play --file PATH  Play a file directly\n"
            "  decode IN OUT.wav Decode a file to WAV (no audio device needed)\n"
            "  pause|resume|stop|next|prev|shuffle|status\n"
            "  ui                Terminal UI\n"
            "  gui               Graphical UI on the framebuffer\n\n"
            "play blocks until the track ends; --no-wait returns immediately.\n"
            "Mount: --mount PATH or TINYPOD_MOUNT. On N31, auto-detects /mnt/disk.\n");
}

/* Optional FB UI entry */
int tp_ui_fb_run(struct tp_app *app);

int main(int argc, char **argv)
{
    const char *mount = NULL;
    const char *backend_name = NULL;
    int no_wait = 0;
    enum tp_player_backend backend;
    struct tp_app app;
    int i = 1;
    int rc = 0;
    const char *cmd;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            mount = argv[++i];
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend_name = argv[++i];
        } else if (strcmp(argv[i], "--no-wait") == 0) {
            no_wait = 1;
        } else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "--verbose") == 0) {
            tp_log_set_level(TP_LOG_DEBUG);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage();
            return 2;
        }
        i++;
    }

    if (!backend_name) {
#ifdef TINYPOD_N31
        backend_name = "alsa";
#else
        backend_name = "external";
#endif
    }
    backend = tp_player_backend_from_name(backend_name);

    cmd = (i < argc) ? argv[i++] : "scan";

    /*
     * decode works on a single file and never touches the library, so it must
     * not need a mounted volume: it is the way to check a file decodes when
     * the iPod is not attached, or when there is no audio device at all.
     */
    if (strcmp(cmd, "decode") == 0) {
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        return tp_app_cmd_decode(NULL, argv[i], argv[i + 1]);
    }

    /*
     * Likewise "play --file": one named file, no library lookup. This is how
     * a track gets played straight off the disk before the FTL mount is up.
     */
    if (strcmp(cmd, "play") == 0 && i + 1 < argc && strcmp(argv[i], "--file") == 0) {
        struct tp_player *pl = tp_player_create(backend);

        if (!pl)
            return 1;
        rc = tp_player_play_file(pl, argv[i + 1]) == 0 ? 0 : 1;
        if (rc == 0 && !no_wait && backend != TP_PLAYER_NULL) {
            if (tp_player_wait(pl) != 0) {
                const char *e = tp_player_last_error(pl);
                if (e && e[0])
                    fprintf(stderr, "playback stopped: %s\n", e);
                rc = 1;
            }
        }
        tp_player_destroy(pl);
        return rc;
    }

    if (tp_app_init(&app, mount, backend) != 0)
        return 1;

    if (strcmp(cmd, "scan") == 0) {
        rc = tp_app_cmd_scan(&app);
    } else if (strcmp(cmd, "list") == 0) {
        rc = tp_app_cmd_list(&app, (i < argc) ? argv[i] : NULL);
    } else if (strcmp(cmd, "search") == 0) {
        if (i >= argc) {
            usage();
            rc = 2;
        } else {
            rc = tp_app_cmd_search(&app, argv[i]);
        }
    } else if (strcmp(cmd, "libcheck") == 0) {
        int json = 0;
        if (i < argc && strcmp(argv[i], "--json") == 0)
            json = 1;
        rc = tp_app_cmd_libcheck(&app, json);
    } else if (strcmp(cmd, "export-json") == 0 || strcmp(cmd, "db-dump") == 0) {
        rc = tp_app_cmd_export_json(&app, stdout);
    } else if (strcmp(cmd, "export-m3u") == 0) {
        rc = tp_app_cmd_export_m3u(&app, stdout);
    } else if (strcmp(cmd, "play") == 0) {
        if (i < argc) {
            uint64_t id = strtoull(argv[i], NULL, 10);
            rc = tp_app_cmd_play_id(&app, id);
        } else {
            usage();
            rc = 2;
        }
        /*
         * Teardown stops playback, so returning here would end the track the
         * instant it started. Hold until it finishes unless asked not to.
         */
        if (rc == 0 && !no_wait && backend != TP_PLAYER_NULL) {
            if (tp_player_wait(app.player) != 0) {
                const char *e = tp_player_last_error(app.player);
                if (e && e[0])
                    fprintf(stderr, "playback stopped: %s\n", e);
                rc = 1;
            }
        }
    } else if (strcmp(cmd, "decode") == 0) {
        if (i + 1 < argc) {
            rc = tp_app_cmd_decode(&app, argv[i], argv[i + 1]);
        } else {
            usage();
            rc = 2;
        }
    } else if (strcmp(cmd, "pause") == 0) {
        rc = tp_app_cmd_pause(&app);
    } else if (strcmp(cmd, "resume") == 0) {
        rc = tp_app_cmd_resume(&app);
    } else if (strcmp(cmd, "stop") == 0) {
        rc = tp_app_cmd_stop(&app);
    } else if (strcmp(cmd, "next") == 0) {
        rc = tp_app_cmd_next(&app);
    } else if (strcmp(cmd, "prev") == 0) {
        rc = tp_app_cmd_prev(&app);
    } else if (strcmp(cmd, "shuffle") == 0) {
        rc = tp_app_cmd_shuffle(&app);
    } else if (strcmp(cmd, "status") == 0) {
        rc = tp_app_cmd_status(&app);
    } else if (strcmp(cmd, "ui") == 0) {
        if (!app.loaded)
            tp_app_load(&app);
        rc = tp_ui_fb_run(&app);
    } else if (strcmp(cmd, "gui") == 0) {
#ifdef TP_WITH_LVGL
        if (!app.loaded)
            tp_app_load(&app);
        rc = tp_lv_ui_run(&app, NULL);
#else
        fprintf(stderr,
                "This build has no graphical UI.\n"
                "Rebuild with UI_LVGL=1 LVGL=/path/to/lvgl, or use `ui`.\n");
        rc = 2;
#endif
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage();
        rc = 2;
    }

    tp_app_free(&app);
    return rc;
}
