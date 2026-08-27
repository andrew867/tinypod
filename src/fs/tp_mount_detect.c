#include "tp_mount.h"
#include "tp_util.h"
#include "tp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_known_mounts[] = {
    "/mnt/disk",
    "/mnt/ipod",
    "/media/ipod",
    "/run/media/ipod",
    NULL
};

void tp_volume_free(struct tp_volume *v)
{
    if (!v)
        return;
    free(v->mount_root);
    free(v->ipod_control_root);
    v->mount_root = NULL;
    v->ipod_control_root = NULL;
}

static int dir_ok(const char *p)
{
    return tp_is_dir(p);
}

static enum tp_vol_health assess(const char *ipod_control)
{
    char *itunes, *music;
    enum tp_vol_health h = TP_VOL_OK;
    if (!dir_ok(ipod_control))
        return TP_VOL_NO_IPOD_CONTROL;
    itunes = tp_path_join2(ipod_control, "iTunes");
    music = tp_path_join2(ipod_control, "Music");
    if (!itunes || !music) {
        free(itunes);
        free(music);
        return TP_VOL_READ_ERROR;
    }
    if (!dir_ok(itunes) && !dir_ok(music))
        h = TP_VOL_EMPTY;
    else if (!dir_ok(itunes))
        h = TP_VOL_NO_ITUNES_DIR;
    else if (!dir_ok(music))
        h = TP_VOL_NO_MUSIC_DIR;
    free(itunes);
    free(music);
    return h;
}

static int try_set_from_ipod_control(struct tp_volume *out, const char *ipod)
{
    char *parent = NULL;
    const char *slash;
    enum tp_vol_health h;
    if (!dir_ok(ipod))
        return -1;
    h = assess(ipod);
    if (h == TP_VOL_NO_IPOD_CONTROL)
        return -1;
    slash = strrchr(ipod, '/');
    if (slash && slash != ipod) {
        parent = tp_strndup(ipod, (size_t)(slash - ipod));
    } else {
        parent = tp_strdup("/");
    }
    if (!parent)
        return -1;
    out->ipod_control_root = tp_strdup(ipod);
    out->mount_root = parent;
    out->health = h;
    return out->ipod_control_root ? 0 : -1;
}

static int try_set_from_mount(struct tp_volume *out, const char *mount)
{
    char *ipod;
    enum tp_vol_health h;
    if (!dir_ok(mount))
        return -1;
    ipod = tp_path_join2(mount, "iPod_Control");
    if (!ipod)
        return -1;
    if (dir_ok(ipod)) {
        h = assess(ipod);
        out->mount_root = tp_strdup(mount);
        out->ipod_control_root = ipod;
        out->health = h;
        return out->mount_root ? 0 : -1;
    }
    free(ipod);
    /* Maybe mount itself is iPod_Control */
    if (dir_ok(tp_path_join2(mount, "iTunes")) || dir_ok(tp_path_join2(mount, "Music"))) {
        /* leak-safe: check by constructing once */
        char *it = tp_path_join2(mount, "iTunes");
        char *mu = tp_path_join2(mount, "Music");
        int ok = (it && dir_ok(it)) || (mu && dir_ok(mu));
        free(it);
        free(mu);
        if (ok)
            return try_set_from_ipod_control(out, mount);
    }
    return -1;
}

static int looks_like_ipod_control(const char *path)
{
    char *it, *mu;
    int ok;
    it = tp_path_join2(path, "iTunes");
    mu = tp_path_join2(path, "Music");
    ok = (it && dir_ok(it)) || (mu && dir_ok(mu));
    free(it);
    free(mu);
    return ok;
}

static int probe_proc_mounts(struct tp_volume *out)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[1024];
    char dev[256], mnt[512], type[64];
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%255s %511s %63s", dev, mnt, type) < 3)
            continue;
        (void)dev;
        (void)type;
        if (try_set_from_mount(out, mnt) == 0) {
            if (out->health == TP_VOL_OK || out->health == TP_VOL_NO_ITUNES_DIR ||
                out->health == TP_VOL_NO_MUSIC_DIR) {
                fclose(f);
                return 0;
            }
            tp_volume_free(out);
            memset(out, 0, sizeof(*out));
        }
    }
    fclose(f);
    return -1;
}

int tp_mount_detect(const char *cli_mount, struct tp_volume *out)
{
    const char *env;
    size_t i;

    memset(out, 0, sizeof(*out));
    out->health = TP_VOL_NOT_FOUND;

    if (cli_mount && cli_mount[0]) {
        if (looks_like_ipod_control(cli_mount)) {
            if (try_set_from_ipod_control(out, cli_mount) == 0)
                return 0;
        }
        if (try_set_from_mount(out, cli_mount) == 0)
            return 0;
        out->health = TP_VOL_NOT_FOUND;
        return -1;
    }

    env = getenv("TINYPOD_MOUNT");
    if (env && env[0]) {
        if (looks_like_ipod_control(env)) {
            if (try_set_from_ipod_control(out, env) == 0)
                return 0;
        }
        if (try_set_from_mount(out, env) == 0)
            return 0;
    }

    for (i = 0; k_known_mounts[i]; i++) {
        if (try_set_from_mount(out, k_known_mounts[i]) == 0)
            return 0;
    }

    if (probe_proc_mounts(out) == 0)
        return 0;

    out->health = TP_VOL_NOT_FOUND;
    return -1;
}
