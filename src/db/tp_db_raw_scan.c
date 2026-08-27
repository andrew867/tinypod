#include "tp_db.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_path.h"
#include "tp_file_probe.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal ID3v2 text frame reader */
static char *id3_get(const char *path, const char *frame4)
{
    FILE *f;
    unsigned char hdr[10];
    unsigned char fhdr[10];
    unsigned size, fsize;
    char *out = NULL;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) {
        fclose(f);
        return NULL;
    }
    size = ((hdr[6] & 0x7f) << 21) | ((hdr[7] & 0x7f) << 14) |
           ((hdr[8] & 0x7f) << 7) | (hdr[9] & 0x7f);
    while (size >= 10 && fread(fhdr, 1, 10, f) == 10) {
        if (fhdr[0] == 0)
            break;
        fsize = ((unsigned)fhdr[4] << 24) | ((unsigned)fhdr[5] << 16) |
                ((unsigned)fhdr[6] << 8) | (unsigned)fhdr[7];
        if (fsize > size || fsize > 1024 * 1024)
            break;
        if (memcmp(fhdr, frame4, 4) == 0 && fsize > 1) {
            unsigned char enc;
            char *buf = (char *)malloc(fsize);
            if (!buf)
                break;
            if (fread(buf, 1, fsize, f) != fsize) {
                free(buf);
                break;
            }
            enc = (unsigned char)buf[0];
            if (enc == 0 || enc == 3) {
                out = tp_strndup(buf + 1, fsize - 1);
            } else if (enc == 1 && fsize >= 3) {
                /* UTF-16 with BOM — best-effort ASCII-ish */
                size_t i, o = 0;
                out = (char *)malloc(fsize);
                if (out) {
                    for (i = 3; i + 1 < fsize; i += 2) {
                        unsigned char lo = (unsigned char)buf[i];
                        if (lo == 0)
                            break;
                        if (lo < 0x80)
                            out[o++] = (char)lo;
                    }
                    out[o] = '\0';
                }
            }
            free(buf);
            break;
        } else {
            if (fseek(f, (long)fsize, SEEK_CUR) != 0)
                break;
        }
        size -= 10 + fsize;
    }
    fclose(f);
    return out;
}

/* Minimal MP4 ilst ©nam/©ART/©alb */
static char *mp4_data_string(FILE *f, unsigned atom_size)
{
    unsigned char *buf;
    char *out = NULL;
    unsigned i;
    if (atom_size < 16 || atom_size > 1024 * 1024)
        return NULL;
    buf = (unsigned char *)malloc(atom_size);
    if (!buf)
        return NULL;
    if (fread(buf, 1, atom_size, f) != atom_size) {
        free(buf);
        return NULL;
    }
    /* find 'data' */
    for (i = 0; i + 8 < atom_size; i++) {
        if (memcmp(buf + i + 4, "data", 4) == 0) {
            unsigned dsz = ((unsigned)buf[i] << 24) | ((unsigned)buf[i + 1] << 16) |
                           ((unsigned)buf[i + 2] << 8) | (unsigned)buf[i + 3];
            if (i + dsz <= atom_size && dsz > 16) {
                out = tp_strndup((char *)buf + i + 16, dsz - 16);
            }
            break;
        }
    }
    free(buf);
    return out;
}

static void mp4_scan(const char *path, char **title, char **artist, char **album)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[8];
    long limit = 2 * 1024 * 1024;
    long pos = 0;
    if (!f)
        return;
    while (pos < limit && fread(hdr, 1, 8, f) == 8) {
        unsigned sz = ((unsigned)hdr[0] << 24) | ((unsigned)hdr[1] << 16) |
                      ((unsigned)hdr[2] << 8) | (unsigned)hdr[3];
        char typ[5];
        memcpy(typ, hdr + 4, 4);
        typ[4] = '\0';
        if (sz < 8)
            break;
        if (strcmp(typ, "moov") == 0 || strcmp(typ, "udta") == 0 ||
            strcmp(typ, "meta") == 0 || strcmp(typ, "ilst") == 0) {
            /* descend: stay in place after header; meta has 4 byte version */
            pos += 8;
            if (strcmp(typ, "meta") == 0) {
                fseek(f, 4, SEEK_CUR);
                pos += 4;
            }
            continue;
        }
        if (typ[0] == (char)0xa9 && typ[1] == 'n' && typ[2] == 'a' && typ[3] == 'm' &&
            !*title)
            *title = mp4_data_string(f, sz - 8);
        else if (typ[0] == (char)0xa9 && typ[1] == 'A' && typ[2] == 'R' && typ[3] == 'T' &&
                 !*artist)
            *artist = mp4_data_string(f, sz - 8);
        else if (typ[0] == (char)0xa9 && typ[1] == 'a' && typ[2] == 'l' && typ[3] == 'b' &&
                 !*album)
            *album = mp4_data_string(f, sz - 8);
        else
            fseek(f, (long)(sz - 8), SEEK_CUR);
        pos += sz;
        if (*title && *artist && *album)
            break;
    }
    fclose(f);
}

static int is_audio_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.')
        return 0;
    return tp_strcasecmp(dot, ".mp3") == 0 || tp_strcasecmp(dot, ".m4a") == 0 ||
           tp_strcasecmp(dot, ".m4p") == 0 || tp_strcasecmp(dot, ".aac") == 0 ||
           tp_strcasecmp(dot, ".wav") == 0 || tp_strcasecmp(dot, ".aiff") == 0 ||
           tp_strcasecmp(dot, ".aif") == 0;
}

static uint64_t hash_path(const char *s)
{
    uint64_t h = 14695981039346656037ull;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

int tp_db_load_raw(struct tp_library *lib, const char *mount_root,
                   const char *ipod_control_root)
{
    char *music;
    int folder;
    uint64_t id_seq = 1;

    music = tp_path_join2(ipod_control_root, "Music");
    if (!music || !tp_is_dir(music)) {
        free(music);
        return -1;
    }

    for (folder = 0; folder < 100; folder++) {
        char fname[8];
        char *dirpath;
        DIR *d;
        struct dirent *de;
        snprintf(fname, sizeof(fname), "F%02d", folder);
        dirpath = tp_path_join2(music, fname);
        if (!dirpath || !tp_is_dir(dirpath)) {
            free(dirpath);
            continue;
        }
        d = opendir(dirpath);
        if (!d) {
            free(dirpath);
            continue;
        }
        while ((de = readdir(d)) != NULL) {
            struct tp_track t;
            char *rel, *abs;
            enum tp_codec codec;
            enum tp_path_status ps;
            if (!is_audio_name(de->d_name))
                continue;
            memset(&t, 0, sizeof(t));
            rel = tp_path_join3("iPod_Control/Music", fname, de->d_name);
            abs = tp_path_join2(dirpath, de->d_name);
            t.track_id = hash_path(rel ? rel : de->d_name);
            if (t.track_id == 0)
                t.track_id = id_seq++;
            t.ipod_path = rel;
            ps = tp_path_resolve(mount_root, rel, &t.absolute_path);
            if (!t.absolute_path)
                t.absolute_path = abs;
            else
                free(abs);
            t.file_exists = (ps == TP_PATH_OK || ps == TP_PATH_CASEFOLD_MATCH ||
                             tp_file_exists(t.absolute_path));
            t.file_size = tp_file_size(t.absolute_path);
            t.playable_probe_ok =
                (uint8_t)tp_file_probe_playable(t.absolute_path, &codec);
            t.codec = codec;

            if (codec == TP_CODEC_MP3) {
                t.title = id3_get(t.absolute_path, "TIT2");
                t.artist = id3_get(t.absolute_path, "TPE1");
                t.album = id3_get(t.absolute_path, "TALB");
                t.source = (t.title || t.artist) ? TP_SOURCE_RAW_TAG_SCAN
                                                 : TP_SOURCE_RAW_FILENAME;
            } else if (codec == TP_CODEC_AAC || codec == TP_CODEC_ALAC) {
                mp4_scan(t.absolute_path, &t.title, &t.artist, &t.album);
                t.source = (t.title || t.artist) ? TP_SOURCE_RAW_TAG_SCAN
                                                 : TP_SOURCE_RAW_FILENAME;
            } else {
                t.source = TP_SOURCE_RAW_FILENAME;
            }
            if (!t.title || !t.title[0]) {
                free(t.title);
                t.title = tp_strdup(de->d_name);
            }
            if (!t.artist || !t.artist[0]) {
                free(t.artist);
                t.artist = tp_strdup("Unknown Artist");
            }
            if (!t.album || !t.album[0]) {
                free(t.album);
                t.album = tp_strdup("Unknown Album");
            }
            if (tp_library_add_track(lib, &t) != 0)
                tp_track_clear(&t);
            else
                memset(&t, 0, sizeof(t));
        }
        closedir(d);
        free(dirpath);
    }
    free(music);
    lib->format = TP_DB_FORMAT_RAW_SCAN;
    return lib->track_count > 0 ? 0 : -1;
}
