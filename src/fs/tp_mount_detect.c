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

static int looks_like_ipod_control(const char *path);

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

    /* No iPod_Control under it - so perhaps it is one. Same test as
       everywhere else, which also stops this leaking the two paths it used to
       build twice and free once. */
    if (looks_like_ipod_control(mount))
        return try_set_from_ipod_control(out, mount);
    return -1;
}

/* Case-insensitive name compare. vfat is case-preserving and
   case-insensitive, so the directory can come back spelled any way at all. */
static int name_is(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && b[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return a[i] == 0 && b[i] == 0;
}

/*
 * Is this directory itself an iPod_Control?
 *
 * It used to be enough to hold an iTunes folder OR a Music folder. "Music" is
 * what anyone would call a folder of music, so a volume with one at its root
 * was taken for an iPod_Control - and the real iPod_Control next to it was
 * never looked at. That is 496 tracks going missing because somebody copied
 * an album onto the disk.
 *
 * Named iPod_Control is conclusive. Otherwise it takes both: every real
 * iPod_Control has an iTunes directory beside its Music one, and a folder of
 * albums has neither the name nor the pair.
 */
static int looks_like_ipod_control(const char *path)
{
    const char *base;
    char *it, *mu;
    int ok;

    if (!path)
        return 0;

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (name_is(base, "iPod_Control"))
        return 1;

    it = tp_path_join2(path, "iTunes");
    mu = tp_path_join2(path, "Music");
    ok = it && mu && dir_ok(it) && dir_ok(mu);
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

    /*
     * An explicit --mount is tried first and is not the last word.
     *
     * This used to return failure the moment the named path had no library
     * on it, while TINYPOD_MOUNT right below fell through to the rest of the
     * search. The asymmetry was not deliberate and it cost a working
     * library: started from the launcher, TinyPod is handed --mount by
     * n31-autostart, and if the shell's guess is off - the apps directory
     * rather than the volume it sits on, say - the answer was an empty
     * library. Typed by hand with no --mount, the same binary on the same
     * disk found everything.
     *
     * So it is a hint. If it holds a library, that is the answer; if it does
     * not, carry on looking and say so, because ending up somewhere other
     * than where you were told is worth knowing about.
     */
    if (cli_mount && cli_mount[0]) {
        /*
         * <path>/iPod_Control first, and only then whether <path> might be
         * one itself. Finding the real thing beats a heuristic, and the
         * heuristic used to run first purely by accident of ordering.
         */
        if (try_set_from_mount(out, cli_mount) == 0)
            return 0;
        if (looks_like_ipod_control(cli_mount) &&
            try_set_from_ipod_control(out, cli_mount) == 0)
            return 0;
        tp_info("no library at %s - looking elsewhere", cli_mount);
    }

    env = getenv("TINYPOD_MOUNT");
    if (env && env[0]) {
        if (try_set_from_mount(out, env) == 0)
            return 0;
        if (looks_like_ipod_control(env) &&
            try_set_from_ipod_control(out, env) == 0)
            return 0;
    }

    for (i = 0; k_known_mounts[i]; i++) {
        if (try_set_from_mount(out, k_known_mounts[i]) == 0) {
            if (cli_mount && cli_mount[0])
                tp_info("using %s instead", out->mount_root);
            return 0;
        }
    }

    if (probe_proc_mounts(out) == 0) {
        if (cli_mount && cli_mount[0])
            tp_info("using %s instead", out->mount_root);
        return 0;
    }

    out->health = TP_VOL_NOT_FOUND;
    return -1;
}
