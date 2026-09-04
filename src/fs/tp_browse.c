#include "tp_browse.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * What this build can decode, by extension.
 *
 * The lean build is Helix and the MP4 demuxer: AAC in its containers, MP3,
 * and WAV. The wide build adds FFmpeg, which opens nearly anything - so the
 * list grows with the build rather than promising formats that are not
 * linked in. A browser that lists a file it cannot play is offering a choice
 * that ends in an error message.
 */
static const char *const k_ext_lean[] = {
    "mp3", "mp2", "mpga", "m4a", "m4b", "m4p", "mp4", "aac", "adts", "wav",
    NULL
};

#ifdef TP_WITH_FFMPEG
static const char *const k_ext_wide[] = {
    "flac", "ogg", "oga", "opus", "wma", "ape", "wv", "tta", "mpc",
    "ac3", "eac3", "dts", "aiff", "aif", "aifc", "caf", "au", "w64",
    "mka", "dsf", "amr", "voc", "ra", "shn", "spx", "alac",
    NULL
};
#endif

static int ext_eq(const char *ext, const char *want)
{
    size_t i;

    for (i = 0; ext[i] && want[i]; i++) {
        char a = ext[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return ext[i] == 0 && want[i] == 0;
}

int tp_browse_playable(const char *name)
{
    const char *dot = strrchr(name, '.');
    size_t i;

    if (!dot || !dot[1])
        return 0;
    dot++;

    for (i = 0; k_ext_lean[i]; i++)
        if (ext_eq(dot, k_ext_lean[i]))
            return 1;
#ifdef TP_WITH_FFMPEG
    for (i = 0; k_ext_wide[i]; i++)
        if (ext_eq(dot, k_ext_wide[i]))
            return 1;
#endif
    return 0;
}

int tp_browse_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl;

    if (!out || !cap || !dir || !name)
        return -1;

    dl = strlen(dir);
    /* "/" joins to "/thing", not "//thing". */
    if (dl && dir[dl - 1] == '/')
        dl--;

    if (dl + 1 + strlen(name) + 1 > cap)
        return -1;

    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, strlen(name) + 1);
    return 0;
}

int tp_browse_parent(char *out, size_t cap, const char *path)
{
    const char *slash;
    size_t n;

    if (!out || !cap || !path || !path[0])
        return -1;
    if (!strcmp(path, "/"))
        return -1;

    slash = strrchr(path, '/');
    if (!slash || slash == path) {
        /* One level below the root: the parent is the root itself. */
        if (cap < 2) return -1;
        out[0] = '/';
        out[1] = 0;
        return 0;
    }

    n = (size_t)(slash - path);
    if (n + 1 > cap)
        return -1;
    memcpy(out, path, n);
    out[n] = 0;
    return 0;
}

/*
 * Directories first, then files; within each, by name and without regard to
 * case. readdir gives whatever order the filesystem feels like, which on
 * vfat is roughly creation order - fine for a machine and useless to read.
 */
static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int digit(char c)
{
    return c >= '0' && c <= '9';
}

/*
 * Name order, with runs of digits compared as numbers.
 *
 * Character by character puts "Track 10" before "Track 2", which was always
 * wrong on screen and is worse now that the folder IS the running order -
 * an album played from the browser would go 1, 10, 11, 2. So a run of digits
 * on both sides is compared by value: shorter number first, and on equal
 * length the first differing digit decides.
 *
 * Leading zeros are skipped before the lengths are compared, so "02" and "2"
 * are the same track number, which is what the two naming conventions for the
 * same album mean by them.
 */
static int cmp_entry(const void *pa, const void *pb)
{
    const struct tp_browse_entry *a = pa, *b = pb;
    const char *x, *y;

    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;

    x = a->name;
    y = b->name;

    while (*x && *y) {
        if (digit(*x) && digit(*y)) {
            const char *sx, *sy;
            size_t nx, ny;

            while (*x == '0') x++;
            while (*y == '0') y++;

            sx = x;
            sy = y;
            while (digit(*x)) x++;
            while (digit(*y)) y++;

            nx = (size_t)(x - sx);
            ny = (size_t)(y - sy);
            if (nx != ny)
                return nx < ny ? -1 : 1;

            for (; sx < x; sx++, sy++)
                if (*sx != *sy)
                    return *sx < *sy ? -1 : 1;
            continue;
        }

        {
            char ca = lower(*x), cb = lower(*y);

            if (ca != cb)
                return ca < cb ? -1 : 1;
        }
        x++;
        y++;
    }

    if (*x) return 1;
    if (*y) return -1;
    return 0;
}

int tp_browse_open(struct tp_browse *b, const char *path, int show_hidden,
                   char *err, size_t errsz)
{
    DIR *d;
    struct dirent *de;
    size_t cap = 64;

    if (!b || !path)
        return -1;

    memset(b, 0, sizeof *b);
    snprintf(b->path, sizeof b->path, "%s", path);

    d = opendir(path);
    if (!d) {
        if (err && errsz)
            snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }

    b->entries = malloc(cap * sizeof *b->entries);
    if (!b->entries) {
        closedir(d);
        if (err && errsz)
            snprintf(err, errsz, "out of memory");
        return -1;
    }

    while ((de = readdir(d)) != NULL) {
        struct tp_browse_entry *e;
        char full[TP_BROWSE_PATH_MAX];
        struct stat st;
        int is_dir;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (!show_hidden && de->d_name[0] == '.')
            continue;
        if (strlen(de->d_name) >= TP_BROWSE_NAME_MAX)
            continue;

        if (b->count >= TP_BROWSE_MAX) {
            b->truncated = 1;
            break;
        }

        /*
         * stat rather than d_type. vfat reports DT_UNKNOWN through some
         * kernels, and a directory shown as a file is a dead end the user
         * cannot get past.
         */
        if (tp_browse_join(full, sizeof full, path, de->d_name) != 0)
            continue;
        if (stat(full, &st) != 0)
            continue;
        is_dir = S_ISDIR(st.st_mode) ? 1 : 0;

        if (!is_dir && !tp_browse_playable(de->d_name))
            continue;

        if (b->count == cap) {
            size_t ncap = cap * 2;
            struct tp_browse_entry *ne =
                realloc(b->entries, ncap * sizeof *ne);
            if (!ne)
                break;
            b->entries = ne;
            cap = ncap;
        }

        e = &b->entries[b->count++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        e->is_dir = is_dir;
        e->playable = is_dir ? 0 : 1;
    }
    closedir(d);

    if (b->count > 1)
        qsort(b->entries, b->count, sizeof *b->entries, cmp_entry);
    return 0;
}

void tp_browse_free(struct tp_browse *b)
{
    if (!b)
        return;
    free(b->entries);
    b->entries = NULL;
    b->count = 0;
}
