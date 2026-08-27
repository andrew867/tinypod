#include "tp_path.h"
#include "tp_util.h"
#include "tp_log.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void normalize_separators(char *s)
{
    size_t i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '\\' || s[i] == ':')
            s[i] = '/';
    }
}

static void collapse_slashes(char *s)
{
    char *r = s, *w = s;
    int slash = 0;
    while (*r) {
        if (*r == '/') {
            if (!slash) {
                *w++ = '/';
                slash = 1;
            }
        } else {
            *w++ = *r;
            slash = 0;
        }
        r++;
    }
    *w = '\0';
}

static int has_dotdot(const char *s)
{
    const char *p = s;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\0') &&
            (p == s || p[-1] == '/'))
            return 1;
        p++;
    }
    return 0;
}

static char *strip_volume_prefix(char *rel)
{
    /* Remove leading / and optional volume name before iPod_Control */
    char *p = rel;
    while (*p == '/')
        p++;
    if (strncmp(p, "iPod_Control", 12) == 0 ||
        strncmp(p, "iPod_Control/", 13) == 0)
        return p;
    /* Skip first component if not iPod_Control (volume name) */
    {
        char *slash = strchr(p, '/');
        if (slash && strncmp(slash + 1, "iPod_Control", 12) == 0)
            return slash + 1;
    }
    return p;
}

static int path_under_mount(const char *mount, const char *abs)
{
    size_t ml;
    char mbuf[512];
    if (!mount || !abs)
        return 0;
    ml = strlen(mount);
    if (ml >= sizeof(mbuf))
        return 0;
    memcpy(mbuf, mount, ml + 1);
    while (ml > 1 && mbuf[ml - 1] == '/')
        mbuf[--ml] = '\0';
    if (strncmp(abs, mbuf, ml) != 0)
        return 0;
    return abs[ml] == '\0' || abs[ml] == '/';
}

static char *casefold_lookup(const char *want)
{
    char *copy, *tok, *save = NULL;
    char *cur, *next;
    DIR *d;
    struct dirent *de;
    size_t i;

    if (!want || !want[0])
        return NULL;
    if (tp_file_exists(want))
        return tp_strdup(want);

    copy = tp_strdup(want);
    if (!copy)
        return NULL;

    /* Walk components with case-insensitive match */
    if (copy[0] == '/') {
        cur = tp_strdup("/");
        tok = strtok_r(copy + 1, "/", &save);
    } else {
        cur = tp_strdup(".");
        tok = strtok_r(copy, "/", &save);
    }
    if (!cur) {
        free(copy);
        return NULL;
    }

    while (tok) {
        d = opendir(cur);
        if (!d) {
            free(cur);
            free(copy);
            return NULL;
        }
        next = NULL;
        while ((de = readdir(d)) != NULL) {
            if (tp_strcasecmp(de->d_name, tok) == 0) {
                next = tp_path_join2(cur, de->d_name);
                break;
            }
        }
        closedir(d);
        free(cur);
        if (!next) {
            free(copy);
            return NULL;
        }
        cur = next;
        tok = strtok_r(NULL, "/", &save);
        (void)i;
    }
    free(copy);
    return cur;
}

enum tp_path_status tp_path_resolve(const char *mount_root, const char *ipod_path,
                                    char **out_abs)
{
    char *norm, *rel, *abs, *cf;

    *out_abs = NULL;
    if (!mount_root || !ipod_path || !ipod_path[0])
        return TP_PATH_INVALID;

    norm = tp_strdup(ipod_path);
    if (!norm)
        return TP_PATH_INVALID;
    normalize_separators(norm);
    collapse_slashes(norm);

    if (has_dotdot(norm)) {
        free(norm);
        return TP_PATH_INVALID;
    }

    rel = strip_volume_prefix(norm);
    while (*rel == '/')
        rel++;

    abs = tp_path_join2(mount_root, rel);
    free(norm);
    if (!abs)
        return TP_PATH_INVALID;

    if (!path_under_mount(mount_root, abs)) {
        free(abs);
        return TP_PATH_OUTSIDE_MOUNT;
    }

    if (tp_file_exists(abs)) {
        *out_abs = abs;
        return TP_PATH_OK;
    }

    cf = casefold_lookup(abs);
    if (cf && path_under_mount(mount_root, cf) && tp_file_exists(cf)) {
        free(abs);
        *out_abs = cf;
        return TP_PATH_CASEFOLD_MATCH;
    }
    free(cf);
    *out_abs = abs;
    return TP_PATH_MISSING;
}

enum tp_path_status tp_path_resolve_parts(const char *mount_root,
                                          const char *base_rel,
                                          const char *rel_file,
                                          char **out_abs)
{
    char *combined, *b, *r;
    enum tp_path_status st;

    *out_abs = NULL;
    if (!mount_root || !rel_file)
        return TP_PATH_INVALID;

    b = base_rel ? tp_strdup(base_rel) : tp_strdup("");
    r = tp_strdup(rel_file);
    if (!b || !r) {
        free(b);
        free(r);
        return TP_PATH_INVALID;
    }
    normalize_separators(b);
    normalize_separators(r);
    combined = tp_path_join2(b, r);
    free(b);
    free(r);
    if (!combined)
        return TP_PATH_INVALID;
    st = tp_path_resolve(mount_root, combined, out_abs);
    free(combined);
    return st;
}
