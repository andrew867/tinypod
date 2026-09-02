#ifndef TP_DB_H
#define TP_DB_H

#include <stddef.h>
#include <stdint.h>
#include "tp_io.h"

#ifdef __cplusplus
extern "C" {
#endif

enum tp_source {
    TP_SOURCE_CLASSIC_ITUNESDB = 0,
    TP_SOURCE_SQLITE_ITDB,
    TP_SOURCE_RAW_TAG_SCAN,
    TP_SOURCE_RAW_FILENAME
};

enum tp_codec {
    TP_CODEC_UNKNOWN = 0,
    TP_CODEC_MP3,
    TP_CODEC_AAC,
    TP_CODEC_ALAC,
    TP_CODEC_WAV,
    TP_CODEC_AIFF,
    TP_CODEC_PROTECTED_UNSUPPORTED,
    /*
     * The file is there and its contents would not read. Distinct from
     * UNKNOWN, which means "read it and did not recognise it" - that is a
     * format question and this is a storage one, and they were the same
     * answer until the probe stopped guessing from the extension after an
     * I/O error.
     */
    TP_CODEC_UNREADABLE
};

enum tp_db_format {
    TP_DB_FORMAT_UNKNOWN = 0,
    TP_DB_FORMAT_SQLITE_ITDB,
    TP_DB_FORMAT_ITUNESCDB,
    TP_DB_FORMAT_ITUNESDB,
    TP_DB_FORMAT_RAW_SCAN
};

enum tp_vol_health {
    TP_VOL_OK = 0,
    TP_VOL_NO_IPOD_CONTROL,
    TP_VOL_NO_ITUNES_DIR,
    TP_VOL_NO_MUSIC_DIR,
    TP_VOL_READ_ERROR,
    TP_VOL_EMPTY,
    TP_VOL_NOT_FOUND
};

enum tp_path_status {
    TP_PATH_OK = 0,
    TP_PATH_MISSING,
    TP_PATH_OUTSIDE_MOUNT,
    TP_PATH_INVALID,
    TP_PATH_CASEFOLD_MATCH
};

struct tp_track {
    uint64_t track_id;
    char *title;
    char *artist;
    char *album;
    char *album_artist;
    char *genre;
    char *composer;
    uint32_t duration_ms;
    uint32_t track_number;
    uint32_t disc_number;
    uint32_t year;
    uint32_t play_count;
    uint32_t rating;
    char *ipod_path;
    char *absolute_path;
    uint64_t file_size;
    uint8_t file_exists;
    uint8_t playable_probe_ok;
    enum tp_codec codec;
    enum tp_source source;
};

struct tp_playlist {
    uint64_t playlist_id;
    char *name;
    uint64_t *track_ids;
    size_t track_count;
};

struct tp_artist_entry {
    char *name;
    size_t track_count;
};

struct tp_album_entry {
    char *artist;
    char *album;
    size_t track_count;
};

struct tp_genre_entry {
    char *name;
    size_t track_count;
};

struct tp_library_health {
    enum tp_db_format format;
    size_t track_total;
    size_t track_playable;
    size_t missing_files;
    size_t unsupported_codec;
    size_t duplicate_ids;
    size_t path_exact;
    size_t path_casefold;
    size_t path_outside;
    size_t music_folder_count;
    /*
     * Files that are there and would not read. They used to land in
     * track_playable, because the readable check never read anything - so a
     * volume losing blocks reported a clean bill of health right up until
     * something tried to play one.
     */
    size_t unreadable_files;
    char format_name[32];
    char warnings[8][128];
    size_t warning_count;
};

struct tp_library {
    char *mount_root;
    char *ipod_control_root;
    enum tp_db_format format;
    enum tp_vol_health vol_health;

    struct tp_track *tracks;
    size_t track_count;
    size_t track_cap;

    struct tp_playlist *playlists;
    size_t playlist_count;
    size_t playlist_cap;

    struct tp_artist_entry *artists;
    size_t artist_count;
    struct tp_album_entry *albums;
    size_t album_count;
    struct tp_genre_entry *genres;
    size_t genre_count;

    struct tp_library_health health;

    /* Where the load failed, when it did. Kept rather than reduced to -1. */
    struct tp_io_err io_err;
};

void tp_track_clear(struct tp_track *t);
void tp_library_init(struct tp_library *lib);
void tp_library_free(struct tp_library *lib);
int tp_library_add_track(struct tp_library *lib, const struct tp_track *src);
int tp_library_build_indexes(struct tp_library *lib);
struct tp_track *tp_library_find_track(struct tp_library *lib, uint64_t id);
const char *tp_source_name(enum tp_source s);
const char *tp_codec_name(enum tp_codec c);
const char *tp_db_format_name(enum tp_db_format f);
const char *tp_vol_health_name(enum tp_vol_health h);

/* Load library from a discovered volume (ipod_control_root under mount). */
int tp_db_load(struct tp_library *lib, const char *mount_root,
               const char *ipod_control_root, enum tp_db_format prefer);

int tp_db_detect(const char *ipod_control_root, enum tp_db_format *out_fmt,
                 char *json_buf, size_t json_cap);

int tp_db_load_sqlite(struct tp_library *lib, const char *mount_root,
                      const char *ipod_control_root);
int tp_db_load_classic(struct tp_library *lib, const char *mount_root,
                       const char *ipod_control_root);
int tp_db_load_raw(struct tp_library *lib, const char *mount_root,
                   const char *ipod_control_root);

void tp_db_validate(struct tp_library *lib);

#ifdef __cplusplus
}
#endif

#endif /* TP_DB_H */
