#include "tp_db.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_file_probe.h"
#include "tp_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tp_track_clear(struct tp_track *t)
{
    if (!t)
        return;
    free(t->title);
    free(t->artist);
    free(t->album);
    free(t->album_artist);
    free(t->genre);
    free(t->composer);
    free(t->ipod_path);
    free(t->absolute_path);
    memset(t, 0, sizeof(*t));
}

void tp_library_init(struct tp_library *lib)
{
    memset(lib, 0, sizeof(*lib));
}

static void free_playlist(struct tp_playlist *p)
{
    free(p->name);
    free(p->track_ids);
    memset(p, 0, sizeof(*p));
}

void tp_library_free(struct tp_library *lib)
{
    size_t i;
    if (!lib)
        return;
    for (i = 0; i < lib->track_count; i++)
        tp_track_clear(&lib->tracks[i]);
    free(lib->tracks);
    for (i = 0; i < lib->playlist_count; i++)
        free_playlist(&lib->playlists[i]);
    free(lib->playlists);
    for (i = 0; i < lib->artist_count; i++)
        free(lib->artists[i].name);
    free(lib->artists);
    for (i = 0; i < lib->album_count; i++) {
        free(lib->albums[i].artist);
        free(lib->albums[i].album);
    }
    free(lib->albums);
    for (i = 0; i < lib->genre_count; i++)
        free(lib->genres[i].name);
    free(lib->genres);
    free(lib->mount_root);
    free(lib->ipod_control_root);
    memset(lib, 0, sizeof(*lib));
}

int tp_library_add_track(struct tp_library *lib, const struct tp_track *src)
{
    struct tp_track *t;
    if (lib->track_count >= lib->track_cap) {
        size_t nc = lib->track_cap ? lib->track_cap * 2 : 64;
        struct tp_track *nt = (struct tp_track *)realloc(lib->tracks, nc * sizeof(*nt));
        if (!nt)
            return -1;
        lib->tracks = nt;
        lib->track_cap = nc;
    }
    t = &lib->tracks[lib->track_count];
    memset(t, 0, sizeof(*t));
    *t = *src;
    /* Deep-copy strings already owned by src — take ownership of pointers from caller pattern:
       Caller fills src with malloc'd strings; we steal them. */
    lib->track_count++;
    return 0;
}

static int ensure_artist(struct tp_library *lib, const char *name)
{
    size_t i;
    const char *n = name && name[0] ? name : "Unknown Artist";
    for (i = 0; i < lib->artist_count; i++) {
        if (tp_strcasecmp(lib->artists[i].name, n) == 0) {
            lib->artists[i].track_count++;
            return 0;
        }
    }
    {
        struct tp_artist_entry *na =
            (struct tp_artist_entry *)realloc(lib->artists,
                                             (lib->artist_count + 1) * sizeof(*na));
        if (!na)
            return -1;
        lib->artists = na;
        lib->artists[lib->artist_count].name = tp_strdup(n);
        lib->artists[lib->artist_count].track_count = 1;
        lib->artist_count++;
    }
    return 0;
}

static int ensure_album(struct tp_library *lib, const char *artist, const char *album)
{
    size_t i;
    const char *a = artist && artist[0] ? artist : "Unknown Artist";
    const char *b = album && album[0] ? album : "Unknown Album";
    for (i = 0; i < lib->album_count; i++) {
        if (tp_strcasecmp(lib->albums[i].artist, a) == 0 &&
            tp_strcasecmp(lib->albums[i].album, b) == 0) {
            lib->albums[i].track_count++;
            return 0;
        }
    }
    {
        struct tp_album_entry *na =
            (struct tp_album_entry *)realloc(lib->albums,
                                            (lib->album_count + 1) * sizeof(*na));
        if (!na)
            return -1;
        lib->albums = na;
        lib->albums[lib->album_count].artist = tp_strdup(a);
        lib->albums[lib->album_count].album = tp_strdup(b);
        lib->albums[lib->album_count].track_count = 1;
        lib->album_count++;
    }
    return 0;
}

static int ensure_genre(struct tp_library *lib, const char *genre)
{
    size_t i;
    const char *g = genre && genre[0] ? genre : NULL;
    if (!g)
        return 0;
    for (i = 0; i < lib->genre_count; i++) {
        if (tp_strcasecmp(lib->genres[i].name, g) == 0) {
            lib->genres[i].track_count++;
            return 0;
        }
    }
    {
        struct tp_genre_entry *na =
            (struct tp_genre_entry *)realloc(lib->genres,
                                            (lib->genre_count + 1) * sizeof(*na));
        if (!na)
            return -1;
        lib->genres = na;
        lib->genres[lib->genre_count].name = tp_strdup(g);
        lib->genres[lib->genre_count].track_count = 1;
        lib->genre_count++;
    }
    return 0;
}

int tp_library_build_indexes(struct tp_library *lib)
{
    size_t i;
    for (i = 0; i < lib->artist_count; i++)
        free(lib->artists[i].name);
    free(lib->artists);
    lib->artists = NULL;
    lib->artist_count = 0;
    for (i = 0; i < lib->album_count; i++) {
        free(lib->albums[i].artist);
        free(lib->albums[i].album);
    }
    free(lib->albums);
    lib->albums = NULL;
    lib->album_count = 0;
    for (i = 0; i < lib->genre_count; i++)
        free(lib->genres[i].name);
    free(lib->genres);
    lib->genres = NULL;
    lib->genre_count = 0;

    for (i = 0; i < lib->track_count; i++) {
        ensure_artist(lib, lib->tracks[i].artist);
        ensure_album(lib, lib->tracks[i].artist, lib->tracks[i].album);
        ensure_genre(lib, lib->tracks[i].genre);
    }
    tp_sort_tracks_artist_album(lib->tracks, lib->track_count);
    /* The derived lists too: they were appended to in track order, which on a
       real library is no order at all. */
    tp_sort_artists(lib->artists, lib->artist_count);
    tp_sort_albums(lib->albums, lib->album_count);
    return 0;
}

struct tp_track *tp_library_find_track(struct tp_library *lib, uint64_t id)
{
    size_t i;
    for (i = 0; i < lib->track_count; i++) {
        if (lib->tracks[i].track_id == id)
            return &lib->tracks[i];
    }
    return NULL;
}

const char *tp_source_name(enum tp_source s)
{
    switch (s) {
    case TP_SOURCE_CLASSIC_ITUNESDB: return "classic-itunesdb";
    case TP_SOURCE_SQLITE_ITDB: return "sqlite-itdb";
    case TP_SOURCE_RAW_TAG_SCAN: return "raw-tag-scan";
    case TP_SOURCE_RAW_FILENAME: return "raw-filename";
    default: return "unknown";
    }
}

const char *tp_codec_name(enum tp_codec c)
{
    switch (c) {
    case TP_CODEC_MP3: return "mp3";
    case TP_CODEC_AAC: return "aac";
    case TP_CODEC_ALAC: return "alac";
    case TP_CODEC_WAV: return "wav";
    case TP_CODEC_AIFF: return "aiff";
    case TP_CODEC_PROTECTED_UNSUPPORTED: return "protected";
    default: return "unknown";
    }
}

const char *tp_db_format_name(enum tp_db_format f)
{
    switch (f) {
    case TP_DB_FORMAT_SQLITE_ITDB: return "sqlite-itdb";
    case TP_DB_FORMAT_ITUNESCDB: return "itunescdb";
    case TP_DB_FORMAT_ITUNESDB: return "itunesdb";
    case TP_DB_FORMAT_RAW_SCAN: return "raw-scan";
    default: return "unknown";
    }
}

const char *tp_vol_health_name(enum tp_vol_health h)
{
    switch (h) {
    case TP_VOL_OK: return "ok";
    case TP_VOL_NO_IPOD_CONTROL: return "no-ipod-control";
    case TP_VOL_NO_ITUNES_DIR: return "no-itunes-dir";
    case TP_VOL_NO_MUSIC_DIR: return "no-music-dir";
    case TP_VOL_READ_ERROR: return "read-error";
    case TP_VOL_EMPTY: return "empty";
    case TP_VOL_NOT_FOUND: return "not-found";
    default: return "unknown";
    }
}

void tp_db_validate(struct tp_library *lib)
{
    size_t i, j;
    struct tp_library_health *h = &lib->health;
    size_t music_folders = h->music_folder_count;
    memset(h, 0, sizeof(*h));
    h->music_folder_count = music_folders;
    h->format = lib->format;
    snprintf(h->format_name, sizeof(h->format_name), "%s", tp_db_format_name(lib->format));
    h->track_total = lib->track_count;

    for (i = 0; i < lib->track_count; i++) {
        struct tp_track *t = &lib->tracks[i];
        if (t->file_exists)
            h->path_exact++;
        if (t->file_exists && t->playable_probe_ok)
            h->track_playable++;
        if (!t->file_exists)
            h->missing_files++;
        if (t->codec == TP_CODEC_PROTECTED_UNSUPPORTED ||
            (t->file_exists && !t->playable_probe_ok))
            h->unsupported_codec++;
        for (j = i + 1; j < lib->track_count; j++) {
            if (lib->tracks[j].track_id == t->track_id) {
                h->duplicate_ids++;
                break;
            }
        }
    }
}

int tp_db_load(struct tp_library *lib, const char *mount_root,
               const char *ipod_control_root, enum tp_db_format prefer)
{
    enum tp_db_format fmt = prefer;
    int rc;

    if (!lib || !mount_root || !ipod_control_root)
        return -1;

    lib->mount_root = tp_strdup(mount_root);
    lib->ipod_control_root = tp_strdup(ipod_control_root);

    if (fmt == TP_DB_FORMAT_UNKNOWN)
        tp_db_detect(ipod_control_root, &fmt, NULL, 0);

    lib->format = fmt;
    switch (fmt) {
    case TP_DB_FORMAT_SQLITE_ITDB:
        rc = tp_db_load_sqlite(lib, mount_root, ipod_control_root);
        break;
    case TP_DB_FORMAT_ITUNESCDB:
    case TP_DB_FORMAT_ITUNESDB:
        rc = tp_db_load_classic(lib, mount_root, ipod_control_root);
        break;
    default:
        rc = tp_db_load_raw(lib, mount_root, ipod_control_root);
        lib->format = TP_DB_FORMAT_RAW_SCAN;
        break;
    }

    if (rc != 0 && fmt != TP_DB_FORMAT_RAW_SCAN) {
        tp_warn("primary library parse failed; falling back to raw scan");
        tp_library_free(lib);
        tp_library_init(lib);
        lib->mount_root = tp_strdup(mount_root);
        lib->ipod_control_root = tp_strdup(ipod_control_root);
        rc = tp_db_load_raw(lib, mount_root, ipod_control_root);
        lib->format = TP_DB_FORMAT_RAW_SCAN;
    }

    if (rc == 0) {
        tp_library_build_indexes(lib);
        tp_db_validate(lib);
    }
    return rc;
}
