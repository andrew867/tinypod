#include "tp_db.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_path.h"
#include "tp_file_probe.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Classic iTunesDB / iTunesCDB (mhbd) conservative parser. */

static uint32_t r32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t r16le(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/*
 * Read a whole file, saying which step failed rather than -1 five times.
 *
 * "Could not open it", "it is shorter than a header", "the storage stopped
 * halfway through" and "out of memory" are four different problems with four
 * different answers, and they used to arrive as the same number.
 */
static int load_file(const char *path, unsigned char **out, size_t *out_len,
                     const char *reader, struct tp_io_err *ioe)
{
    FILE *f;
    long sz;
    size_t got;
    unsigned char *buf;

    errno = 0;
    f = fopen(path, "rb");
    if (!f) {
        tp_io_err_set(ioe, reader, "open", path, -1, 0, 0, errno);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        tp_io_err_set(ioe, reader, "seek", path, -1, 0, 0, errno);
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 16) {
        tp_io_err_set(ioe, reader, "too short", path, 0, 16, sz, 0);
        fclose(f);
        return -1;
    }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) {
        tp_io_err_set(ioe, reader, "allocate", path, -1, sz, 0, ENOMEM);
        fclose(f);
        return -1;
    }
    errno = 0;
    got = fread(buf, 1, (size_t)sz, f);
    if (got != (size_t)sz) {
        tp_io_err_set(ioe, reader, "read", path, (long long)got,
                      sz, (long long)got, ferror(f) ? (errno ? errno : EIO) : 0);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

static int maybe_inflate(unsigned char **buf, size_t *len)
{
    /* N7G iTunesCDB is raw mhbd. Compressed variants can be added later. */
    (void)buf;
    (void)len;
    return 0;
}

static char *utf16le_to_utf8(const unsigned char *p, size_t nbytes)
{
    size_t nchars = nbytes / 2;
    size_t i, o = 0;
    char *out = (char *)malloc(nchars * 3 + 1);
    if (!out)
        return NULL;
    for (i = 0; i < nchars; i++) {
        uint16_t u = r16le(p + i * 2);
        if (u == 0)
            break;
        if (u < 0x80) {
            out[o++] = (char)u;
        } else if (u < 0x800) {
            out[o++] = (char)(0xC0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

struct parse_ctx {
    struct tp_library *lib;
    const char *mount_root;
    struct tp_track cur;
    int in_track;
};

static void emit_track(struct parse_ctx *ctx)
{
    enum tp_path_status ps;
    enum tp_codec codec;
    if (!ctx->in_track)
        return;
    ctx->cur.source = TP_SOURCE_CLASSIC_ITUNESDB;
    if (!ctx->cur.title)
        ctx->cur.title = tp_strdup("Unknown Title");
    if (!ctx->cur.artist)
        ctx->cur.artist = tp_strdup("Unknown Artist");
    if (!ctx->cur.album)
        ctx->cur.album = tp_strdup("Unknown Album");
    if (ctx->cur.ipod_path) {
        ps = tp_path_resolve(ctx->mount_root, ctx->cur.ipod_path, &ctx->cur.absolute_path);
        if (ps == TP_PATH_OK || ps == TP_PATH_CASEFOLD_MATCH) {
            ctx->cur.file_exists = 1;
            ctx->cur.file_size = tp_file_size(ctx->cur.absolute_path);
            ctx->cur.playable_probe_ok =
                (uint8_t)tp_file_probe_playable(ctx->cur.absolute_path, &codec);
            ctx->cur.codec = codec;
        }
    }
    if (tp_library_add_track(ctx->lib, &ctx->cur) != 0)
        tp_track_clear(&ctx->cur);
    memset(&ctx->cur, 0, sizeof(ctx->cur));
    ctx->in_track = 0;
}

static void apply_mhod(struct parse_ctx *ctx, uint32_t type, char *text)
{
    if (!text)
        return;
    switch (type) {
    case 1:
        free(ctx->cur.title);
        ctx->cur.title = text;
        break;
    case 2:
        free(ctx->cur.ipod_path);
        ctx->cur.ipod_path = text;
        break;
    case 3:
        free(ctx->cur.album);
        ctx->cur.album = text;
        break;
    case 4:
        free(ctx->cur.artist);
        ctx->cur.artist = text;
        break;
    case 5:
        free(ctx->cur.genre);
        ctx->cur.genre = text;
        break;
    case 12:
        free(ctx->cur.composer);
        ctx->cur.composer = text;
        break;
    default:
        free(text);
        break;
    }
}

static int parse_mhod(struct parse_ctx *ctx, const unsigned char *p, size_t rem)
{
    uint32_t hlen, tlen, type, strlen_b;
    char *text;
    if (rem < 24 || memcmp(p, "mhod", 4) != 0)
        return -1;
    hlen = r32le(p + 4);
    tlen = r32le(p + 8);
    type = r32le(p + 12);
    if (hlen < 24 || tlen < hlen || tlen > rem || tlen > 16 * 1024 * 1024)
        return -1;
    /* string mhod types typically: offset 24: position, length, ... utf16 */
    if (type >= 1 && type <= 50 && tlen >= 40) {
        strlen_b = r32le(p + 28);
        if (40 + strlen_b <= tlen && strlen_b < 1024 * 1024) {
            text = utf16le_to_utf8(p + 40, strlen_b);
            apply_mhod(ctx, type, text);
        }
    }
    return (int)tlen;
}

static int parse_mhit(struct parse_ctx *ctx, const unsigned char *p, size_t rem)
{
    uint32_t hlen, tlen;
    size_t off;
    int n;
    if (rem < 16 || memcmp(p, "mhit", 4) != 0)
        return -1;
    hlen = r32le(p + 4);
    tlen = r32le(p + 8);
    if (hlen < 16 || tlen < hlen || tlen > rem || tlen > 32 * 1024 * 1024)
        return -1;

    emit_track(ctx);
    memset(&ctx->cur, 0, sizeof(ctx->cur));
    ctx->in_track = 1;
    if (hlen >= 28)
        ctx->cur.track_id = r32le(p + 16);
    if (hlen >= 56)
        ctx->cur.duration_ms = r32le(p + 32); /* often ms at various offsets — best effort */
    if (hlen >= 64)
        ctx->cur.track_number = r32le(p + 44) >> 16; /* packed on some versions */
    if (hlen >= 92)
        ctx->cur.year = r32le(p + 88);

    off = hlen;
    while (off + 8 < tlen) {
        if (memcmp(p + off, "mhod", 4) == 0) {
            n = parse_mhod(ctx, p + off, tlen - off);
            if (n <= 0)
                break;
            off += (size_t)n;
        } else {
            break;
        }
    }
    return (int)tlen;
}

static int walk_db(struct parse_ctx *ctx, const unsigned char *p, size_t len)
{
    size_t off = 0;
    while (off + 8 <= len) {
        uint32_t hlen, tlen;
        int n;
        if (off + 12 > len)
            break;
        hlen = r32le(p + off + 4);
        tlen = r32le(p + off + 8);
        if (hlen < 8 || tlen < hlen || off + tlen > len || tlen > 64 * 1024 * 1024) {
            /* skip one byte to resync conservatively for unknown */
            off++;
            continue;
        }
        if (memcmp(p + off, "mhit", 4) == 0) {
            n = parse_mhit(ctx, p + off, len - off);
            if (n > 0)
                off += (size_t)n;
            else
                off += tlen;
        } else if (memcmp(p + off, "mhbd", 4) == 0 || memcmp(p + off, "mhsd", 4) == 0 ||
                   memcmp(p + off, "mhlt", 4) == 0 || memcmp(p + off, "mhyp", 4) == 0 ||
                   memcmp(p + off, "mhip", 4) == 0) {
            /* descend into children after header */
            size_t child = off + hlen;
            size_t end = off + tlen;
            while (child + 8 <= end) {
                uint32_t ch = r32le(p + child + 4);
                uint32_t ct = r32le(p + child + 8);
                if (ch < 8 || ct < ch || child + ct > end)
                    break;
                if (memcmp(p + child, "mhit", 4) == 0) {
                    n = parse_mhit(ctx, p + child, end - child);
                    child += (n > 0) ? (size_t)n : ct;
                } else if (memcmp(p + child, "mhsd", 4) == 0 ||
                           memcmp(p + child, "mhlt", 4) == 0 ||
                           memcmp(p + child, "mhyp", 4) == 0) {
                    /* recursive-ish: advance into subsection by walking */
                    size_t sub = child + ch;
                    size_t send = child + ct;
                    while (sub + 8 <= send) {
                        if (memcmp(p + sub, "mhit", 4) == 0) {
                            n = parse_mhit(ctx, p + sub, send - sub);
                            if (n > 0)
                                sub += (size_t)n;
                            else
                                break;
                        } else {
                            uint32_t sh = r32le(p + sub + 4);
                            uint32_t st = r32le(p + sub + 8);
                            if (sh < 8 || st < sh || sub + st > send)
                                break;
                            if (memcmp(p + sub, "mhsd", 4) == 0 ||
                                memcmp(p + sub, "mhlt", 4) == 0) {
                                /* nest one more level */
                                size_t s2 = sub + sh;
                                size_t e2 = sub + st;
                                while (s2 + 8 <= e2) {
                                    if (memcmp(p + s2, "mhit", 4) == 0) {
                                        n = parse_mhit(ctx, p + s2, e2 - s2);
                                        s2 += (n > 0) ? (size_t)n : r32le(p + s2 + 8);
                                    } else {
                                        uint32_t xh = r32le(p + s2 + 4);
                                        uint32_t xt = r32le(p + s2 + 8);
                                        if (xh < 8 || xt < xh || s2 + xt > e2)
                                            break;
                                        s2 += xt;
                                    }
                                }
                                sub += st;
                            } else {
                                sub += st;
                            }
                        }
                    }
                    child += ct;
                } else {
                    child += ct;
                }
            }
            off += tlen;
        } else {
            off += tlen;
        }
    }
    emit_track(ctx);
    return 0;
}

int tp_db_load_classic(struct tp_library *lib, const char *mount_root,
                       const char *ipod_control_root)
{
    char *itunes, *cdb, *idb;
    const char *path = NULL;
    const char *reader = "itunescdb";
    unsigned char *buf = NULL;
    size_t len = 0;
    struct parse_ctx ctx;

    itunes = tp_path_join2(ipod_control_root, "iTunes");
    cdb = tp_path_join2(itunes, "iTunesCDB");
    idb = tp_path_join2(itunes, "iTunesDB");
    free(itunes);

    if (cdb && tp_is_readable_file(cdb)) {
        path = cdb;
        lib->format = TP_DB_FORMAT_ITUNESCDB;
    } else if (idb && tp_is_readable_file(idb)) {
        path = idb;
        lib->format = TP_DB_FORMAT_ITUNESDB;
        reader = "itunesdb";
    }
    if (!path) {
        /*
         * Neither database is there, or neither would read. Those are not
         * the same thing - one means this is not that kind of volume, the
         * other means the storage lost the file - and they arrived as the
         * same -1.
         */
        int e = 0;
        const char *tried = cdb ? cdb : idb;

        if (tried && tp_file_read_check(tried, &e) == TP_FILE_UNREADABLE)
            tp_io_err_set(&lib->io_err, reader, "read", tried, -1, 0, 0, e);
        else
            tp_io_err_set(&lib->io_err, reader, "not present", tried,
                          -1, 0, 0, ENOENT);
        free(cdb);
        free(idb);
        return -1;
    }

    if (load_file(path, &buf, &len, reader, &lib->io_err) != 0) {
        char line[256];

        if (tp_io_err_format(&lib->io_err, line, sizeof line) == 0)
            tp_error("%s", line);
        else
            tp_error("Could not read database file:\n  %s", path);
        free(cdb);
        free(idb);
        return -1;
    }
    maybe_inflate(&buf, &len);
    if (len < 8 || memcmp(buf, "mhbd", 4) != 0) {
        tp_io_err_set(&lib->io_err, reader, "no mhbd header", path,
                      0, 8, (long long)len, 0);
        tp_error("Not a valid iTunesDB/CDB (missing mhbd):\n  %s", path);
        free(buf);
        free(cdb);
        free(idb);
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.lib = lib;
    ctx.mount_root = mount_root;
    walk_db(&ctx, buf, len);

    free(buf);
    free(cdb);
    free(idb);

    /* Parsed, and empty. A different answer from "would not read", and the
       two used to be the same -1. */
    if (lib->track_count == 0) {
        tp_io_err_set(&lib->io_err, reader, "no tracks in it", path,
                      -1, 0, 0, 0);
        return -1;
    }
    return 0;
}
