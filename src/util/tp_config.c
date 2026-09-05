#include "tp_config.h"
#include "tp_util.h"
#include "tp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void tp_config_init(struct tp_config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->repeat, sizeof(c->repeat), "off");
}

void tp_config_free(struct tp_config *c)
{
    free(c->last_mount);
    c->last_mount = NULL;
}

static void ensure_dir(const char *path)
{
    mkdir(path, 0755);
}

static char *config_path(void)
{
    const char *home = getenv("HOME");
    char *dir, *path;
    if (home && home[0]) {
        dir = tp_path_join2(home, ".config/tinypod");
        ensure_dir(tp_path_join2(home, ".config"));
        ensure_dir(dir);
        path = tp_path_join2(dir, "config.json");
        free(dir);
        return path;
    }
    ensure_dir("/tmp/tinypod");
    return tp_strdup("/tmp/tinypod/config.json");
}

int tp_config_save(const struct tp_config *c)
{
    char *path = config_path();
    FILE *f;
    if (!path)
        return -1;
    f = fopen(path, "w");
    if (!f) {
        free(path);
        return -1;
    }
    fprintf(f,
            "{\n"
            "  \"last_mount\": \"%s\",\n"
            "  \"last_track_id\": %llu,\n"
            "  \"last_position_ms\": %u,\n"
            "  \"shuffle\": %s,\n"
            "  \"repeat\": \"%s\",\n"
            "  \"output\": \"%s\"\n"
            "}\n",
            c->last_mount ? c->last_mount : "",
            (unsigned long long)c->last_track_id, c->last_position_ms,
            c->shuffle ? "true" : "false", c->repeat, c->output);
    fclose(f);
    free(path);
    return 0;
}

int tp_config_load(struct tp_config *c)
{
    char *path = config_path();
    FILE *f;
    char buf[1024];
    char *p;
    tp_config_init(c);
    if (!path)
        return -1;
    f = fopen(path, "r");
    free(path);
    if (!f)
        return -1;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    /* simplistic key scan of whole file */
    rewind(f);
    {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
    }
    fclose(f);
    p = strstr(buf, "\"last_mount\"");
    if (p) {
        p = strchr(p + 12, '"');
        if (p) {
            char *q = strchr(p + 1, '"');
            if (q)
                c->last_mount = tp_strndup(p + 1, (size_t)(q - p - 1));
        }
    }
    p = strstr(buf, "\"last_track_id\"");
    if (p)
        c->last_track_id = (uint64_t)strtoull(p + 15 + strspn(p + 15, "\": "), NULL, 10);
    p = strstr(buf, "\"last_position_ms\"");
    if (p)
        c->last_position_ms =
            (uint32_t)strtoul(p + 18 + strspn(p + 18, "\": "), NULL, 10);
    p = strstr(buf, "\"shuffle\"");
    if (p && strstr(p, "true"))
        c->shuffle = 1;
    p = strstr(buf, "\"repeat\"");
    if (p) {
        p = strchr(p + 8, '"');
        if (p) {
            char *q = strchr(p + 1, '"');
            if (q && (size_t)(q - p - 1) < sizeof(c->repeat)) {
                memcpy(c->repeat, p + 1, (size_t)(q - p - 1));
                c->repeat[q - p - 1] = '\0';
            }
        }
    }

    /* Absent in a file written before this existed, which is the ordinary
       case on an upgrade and not a fault: the empty default means "wherever
       the build was told at startup". */
    p = strstr(buf, "\"output\"");
    if (p) {
        p = strchr(p + 8, '"');
        if (p) {
            char *q = strchr(p + 1, '"');
            if (q && (size_t)(q - p - 1) < sizeof(c->output)) {
                memcpy(c->output, p + 1, (size_t)(q - p - 1));
                c->output[q - p - 1] = '\0';
            }
        }
    }
    return 0;
}
