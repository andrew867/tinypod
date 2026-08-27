#include "tp_db.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_path.h"
#include "tp_file_probe.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct base_loc {
    int id;
    char *path;
};

static char *col_text(sqlite3_stmt *st, int i)
{
    const unsigned char *t = sqlite3_column_text(st, i);
    return tp_utf8_sanitize(t ? (const char *)t : "");
}

static int table_exists(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        ok = 1;
    sqlite3_finalize(st);
    return ok;
}

static int column_exists(sqlite3 *db, const char *table, const char *col)
{
    char sql[256];
    sqlite3_stmt *st = NULL;
    int ok = 0;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *n = sqlite3_column_text(st, 1);
        if (n && strcmp((const char *)n, col) == 0) {
            ok = 1;
            break;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

static int open_ro(const char *path, sqlite3 **db)
{
    char uri[1024];
    int rc;
    snprintf(uri, sizeof(uri), "file:%s?mode=ro", path);
    rc = sqlite3_open_v2(uri, db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        tp_error("Could not open database read-only:\n  %s\n  %s", path,
                 *db ? sqlite3_errmsg(*db) : "open failed");
        if (*db) {
            sqlite3_close(*db);
            *db = NULL;
        }
        return -1;
    }
    return 0;
}

static int load_base_locations(sqlite3 *locdb, struct base_loc **out, size_t *nout)
{
    sqlite3_stmt *st = NULL;
    struct base_loc *arr = NULL;
    size_t n = 0, cap = 0;
    if (sqlite3_prepare_v2(locdb, "SELECT id, path FROM base_location", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) {
            size_t nc = cap ? cap * 2 : 4;
            struct base_loc *na = realloc(arr, nc * sizeof(*na));
            if (!na) {
                sqlite3_finalize(st);
                return -1;
            }
            arr = na;
            cap = nc;
        }
        arr[n].id = sqlite3_column_int(st, 0);
        arr[n].path = col_text(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    *out = arr;
    *nout = n;
    return 0;
}

static const char *base_path_for(struct base_loc *bases, size_t nb, int id)
{
    size_t i;
    for (i = 0; i < nb; i++) {
        if (bases[i].id == id)
            return bases[i].path ? bases[i].path : "";
    }
    return "iPod_Control/Music";
}

static void finish_track(struct tp_library *lib, struct tp_track *t,
                         const char *mount_root)
{
    enum tp_path_status ps;
    enum tp_codec codec;

    if (!t->title || !t->title[0]) {
        free(t->title);
        t->title = tp_strdup("Unknown Title");
    }
    if (!t->artist || !t->artist[0]) {
        free(t->artist);
        t->artist = tp_strdup("Unknown Artist");
    }
    if (!t->album || !t->album[0]) {
        free(t->album);
        t->album = tp_strdup("Unknown Album");
    }

    t->source = TP_SOURCE_SQLITE_ITDB;
    if (t->ipod_path) {
        ps = tp_path_resolve(mount_root, t->ipod_path, &t->absolute_path);
        if (ps == TP_PATH_OK || ps == TP_PATH_CASEFOLD_MATCH) {
            t->file_exists = 1;
            t->file_size = tp_file_size(t->absolute_path);
            t->playable_probe_ok = (uint8_t)tp_file_probe_playable(t->absolute_path, &codec);
            t->codec = codec;
        } else {
            t->file_exists = 0;
            t->playable_probe_ok = 0;
            t->codec = TP_CODEC_UNKNOWN;
        }
    }
    if (tp_library_add_track(lib, t) != 0) {
        tp_track_clear(t);
    } else {
        /* ownership transferred */
        memset(t, 0, sizeof(*t));
    }
}

int tp_db_load_sqlite(struct tp_library *lib, const char *mount_root,
                      const char *ipod_control_root)
{
    char *itlp, *libpath, *locpath;
    sqlite3 *libdb = NULL, *locdb = NULL;
    sqlite3_stmt *st = NULL, *lst = NULL;
    struct base_loc *bases = NULL;
    size_t nb = 0, i;
    int rc = -1;
    const char *item_sql;

    itlp = tp_path_join3(ipod_control_root, "iTunes", "iTunes Library.itlp");
    libpath = tp_path_join2(itlp, "Library.itdb");
    locpath = tp_path_join2(itlp, "Locations.itdb");
    free(itlp);
    if (!libpath || !locpath) {
        free(libpath);
        free(locpath);
        return -1;
    }

    if (open_ro(libpath, &libdb) != 0) {
        free(libpath);
        free(locpath);
        return -1;
    }
    if (open_ro(locpath, &locdb) != 0) {
        sqlite3_close(libdb);
        free(libpath);
        free(locpath);
        return -1;
    }

    if (!table_exists(libdb, "item") || !table_exists(locdb, "location")) {
        tp_error("SQLite library missing required tables (item/location)");
        goto out;
    }

    load_base_locations(locdb, &bases, &nb);

    item_sql =
        "SELECT pid, title, artist, album, album_artist, composer, "
        "CAST(total_time_ms AS INTEGER), track_number, disc_number, year "
        "FROM item WHERE is_song = 1 OR is_song IS NULL";

    if (!column_exists(libdb, "item", "is_song")) {
        item_sql =
            "SELECT pid, title, artist, album, album_artist, composer, "
            "CAST(total_time_ms AS INTEGER), track_number, disc_number, year "
            "FROM item";
    }

    if (sqlite3_prepare_v2(libdb, item_sql, -1, &st, NULL) != SQLITE_OK) {
        tp_error("Failed to query item table: %s", sqlite3_errmsg(libdb));
        goto out;
    }

    if (sqlite3_prepare_v2(locdb,
                           "SELECT base_location_id, location, file_size "
                           "FROM location WHERE item_pid = ?1 LIMIT 1",
                           -1, &lst, NULL) != SQLITE_OK) {
        tp_error("Failed to prepare location query: %s", sqlite3_errmsg(locdb));
        goto out;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        struct tp_track t;
        int base_id;
        const char *base_rel;
        char *rel_file;
        memset(&t, 0, sizeof(t));
        t.track_id = (uint64_t)sqlite3_column_int64(st, 0);
        t.title = col_text(st, 1);
        t.artist = col_text(st, 2);
        t.album = col_text(st, 3);
        t.album_artist = col_text(st, 4);
        t.composer = col_text(st, 5);
        t.duration_ms = (uint32_t)sqlite3_column_int(st, 6);
        t.track_number = (uint32_t)sqlite3_column_int(st, 7);
        t.disc_number = (uint32_t)sqlite3_column_int(st, 8);
        t.year = (uint32_t)sqlite3_column_int(st, 9);

        sqlite3_reset(lst);
        sqlite3_clear_bindings(lst);
        sqlite3_bind_int64(lst, 1, (sqlite3_int64)t.track_id);
        if (sqlite3_step(lst) == SQLITE_ROW) {
            base_id = sqlite3_column_int(lst, 0);
            rel_file = col_text(lst, 1);
            t.file_size = (uint64_t)sqlite3_column_int64(lst, 2);
            base_rel = base_path_for(bases, nb, base_id);
            t.ipod_path = tp_path_join2(base_rel, rel_file);
            free(rel_file);
        }

        finish_track(lib, &t, mount_root);
    }

    /* Playlists */
    if (table_exists(libdb, "container") && table_exists(libdb, "item_to_container")) {
        sqlite3_stmt *cst = NULL;
        if (sqlite3_prepare_v2(libdb,
                               "SELECT pid, name FROM container WHERE name IS NOT NULL",
                               -1, &cst, NULL) == SQLITE_OK) {
            while (sqlite3_step(cst) == SQLITE_ROW) {
                struct tp_playlist pl;
                sqlite3_stmt *ist = NULL;
                memset(&pl, 0, sizeof(pl));
                pl.playlist_id = (uint64_t)sqlite3_column_int64(cst, 0);
                pl.name = col_text(cst, 1);
                if (sqlite3_prepare_v2(libdb,
                                       "SELECT item_pid FROM item_to_container "
                                       "WHERE container_pid = ?1 ORDER BY physical_order",
                                       -1, &ist, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(ist, 1, (sqlite3_int64)pl.playlist_id);
                    while (sqlite3_step(ist) == SQLITE_ROW) {
                        uint64_t *ni;
                        ni = realloc(pl.track_ids, (pl.track_count + 1) * sizeof(*ni));
                        if (!ni)
                            break;
                        pl.track_ids = ni;
                        pl.track_ids[pl.track_count++] =
                            (uint64_t)sqlite3_column_int64(ist, 0);
                    }
                    sqlite3_finalize(ist);
                }
                {
                    struct tp_playlist *np =
                        realloc(lib->playlists, (lib->playlist_count + 1) * sizeof(*np));
                    if (np) {
                        lib->playlists = np;
                        lib->playlists[lib->playlist_count++] = pl;
                    } else {
                        free(pl.name);
                        free(pl.track_ids);
                    }
                }
            }
            sqlite3_finalize(cst);
        }
    }

    lib->format = TP_DB_FORMAT_SQLITE_ITDB;
    rc = 0;

out:
    if (st)
        sqlite3_finalize(st);
    if (lst)
        sqlite3_finalize(lst);
    for (i = 0; i < nb; i++)
        free(bases[i].path);
    free(bases);
    if (libdb)
        sqlite3_close(libdb);
    if (locdb)
        sqlite3_close(locdb);
    free(libpath);
    free(locpath);
    return rc;
}
