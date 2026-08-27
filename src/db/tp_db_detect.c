#include "tp_db.h"
#include "tp_util.h"
#include "tp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int file_readable(const char *path)
{
    return tp_is_readable_file(path);
}

int tp_db_detect(const char *ipod_control_root, enum tp_db_format *out_fmt,
                 char *json_buf, size_t json_cap)
{
    char *itunes, *itlp, *libdb, *locdb, *cdb, *idb, *music;
    enum tp_db_format fmt = TP_DB_FORMAT_UNKNOWN;
    size_t off = 0;
    int music_dirs = 0;
    int i;

    if (out_fmt)
        *out_fmt = TP_DB_FORMAT_UNKNOWN;
    if (!ipod_control_root)
        return -1;

    itunes = tp_path_join2(ipod_control_root, "iTunes");
    music = tp_path_join2(ipod_control_root, "Music");
    itlp = tp_path_join2(itunes, "iTunes Library.itlp");
    libdb = tp_path_join2(itlp, "Library.itdb");
    locdb = tp_path_join2(itlp, "Locations.itdb");
    cdb = tp_path_join2(itunes, "iTunesCDB");
    idb = tp_path_join2(itunes, "iTunesDB");

    if (file_readable(libdb)) {
        fmt = TP_DB_FORMAT_SQLITE_ITDB;
    } else if (file_readable(cdb)) {
        fmt = TP_DB_FORMAT_ITUNESCDB;
    } else if (file_readable(idb)) {
        fmt = TP_DB_FORMAT_ITUNESDB;
    } else if (tp_is_dir(music)) {
        fmt = TP_DB_FORMAT_RAW_SCAN;
    }

    for (i = 0; i < 100; i++) {
        char folder[8];
        char *p;
        snprintf(folder, sizeof(folder), "F%02d", i);
        p = tp_path_join2(music, folder);
        if (p && tp_is_dir(p))
            music_dirs++;
        free(p);
    }

    if (out_fmt)
        *out_fmt = fmt;

    if (json_buf && json_cap) {
        json_buf[0] = '\0';
        off = 0;
        snprintf(json_buf + off, json_cap - off,
                 "{\"format\":\"%s\",\"db_files\":[", tp_db_format_name(fmt));
        off = strlen(json_buf);
        {
            int first = 1;
            if (file_readable(libdb)) {
                snprintf(json_buf + off, json_cap - off, "%s\"Library.itdb\"", first ? "" : ",");
                off = strlen(json_buf);
                first = 0;
            }
            if (file_readable(locdb)) {
                snprintf(json_buf + off, json_cap - off, "%s\"Locations.itdb\"", first ? "" : ",");
                off = strlen(json_buf);
                first = 0;
            }
            if (file_readable(cdb)) {
                snprintf(json_buf + off, json_cap - off, "%s\"iTunesCDB\"", first ? "" : ",");
                off = strlen(json_buf);
                first = 0;
            }
            if (file_readable(idb)) {
                snprintf(json_buf + off, json_cap - off, "%s\"iTunesDB\"", first ? "" : ",");
                off = strlen(json_buf);
            }
            (void)first;
        }
        snprintf(json_buf + off, json_cap - off,
                 "],\"music_folder_count\":%d,\"warnings\":[]}", music_dirs);
    }

    free(itunes);
    free(itlp);
    free(libdb);
    free(locdb);
    free(cdb);
    free(idb);
    free(music);
    return fmt == TP_DB_FORMAT_UNKNOWN ? -1 : 0;
}
