#include "tp_util.h"
#include "tp_db.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

char *tp_strdup(const char *s)
{
    size_t n;
    char *d;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

char *tp_strndup(const char *s, size_t n)
{
    char *d;
    size_t i;
    if (!s)
        return NULL;
    for (i = 0; i < n && s[i]; i++)
        ;
    d = (char *)malloc(i + 1);
    if (!d)
        return NULL;
    memcpy(d, s, i);
    d[i] = '\0';
    return d;
}

void *tp_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    return p;
}

void *tp_xcalloc(size_t n, size_t sz)
{
    return calloc(n ? n : 1, sz ? sz : 1);
}

void *tp_xrealloc(void *p, size_t n)
{
    return realloc(p, n ? n : 1);
}

static void strip_slash(char *s)
{
    size_t n;
    if (!s)
        return;
    n = strlen(s);
    while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
        s[--n] = '\0';
    }
}

char *tp_path_join2(const char *a, const char *b)
{
    size_t la, lb;
    char *out;
    int need_slash;
    if (!a || !a[0])
        return tp_strdup(b);
    if (!b || !b[0])
        return tp_strdup(a);
    while (*b == '/' || *b == '\\')
        b++;
    la = strlen(a);
    lb = strlen(b);
    need_slash = !(la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\'));
    out = (char *)malloc(la + lb + 2);
    if (!out)
        return NULL;
    memcpy(out, a, la);
    if (need_slash)
        out[la++] = '/';
    memcpy(out + la, b, lb + 1);
    return out;
}

char *tp_path_join3(const char *a, const char *b, const char *c)
{
    char *ab = tp_path_join2(a, b);
    char *abc;
    if (!ab)
        return NULL;
    abc = tp_path_join2(ab, c);
    free(ab);
    return abc;
}

int tp_file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

int tp_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Readable means a byte came out of it.
 *
 * This used to be stat, fopen, fclose - which asks whether the directory
 * entry exists and whether the file can be opened, and never whether the data
 * is there. On this device that is not a pedantic distinction: the flash
 * translation layer returns EIO for blocks it has lost track of, so a file
 * with no readable content passes stat and open perfectly and fails on the
 * first read, somewhere much later, in a reader that reports a bare -1.
 *
 * Zero-length files are still readable - they are not damaged, just empty -
 * so the read only has to succeed, not return anything.
 */
int tp_is_readable_file(const char *path)
{
    return tp_file_read_check(path, NULL) == TP_FILE_OK;
}

enum tp_file_state tp_file_read_check(const char *path, int *out_errno)
{
    struct stat st;
    unsigned char byte;
    FILE *f;
    size_t n;

    if (out_errno)
        *out_errno = 0;

    if (!path)
        return TP_FILE_MISSING;

    errno = 0;
    if (stat(path, &st) != 0) {
        if (out_errno) *out_errno = errno;
        return errno == EIO ? TP_FILE_UNREADABLE : TP_FILE_MISSING;
    }
    if (!S_ISREG(st.st_mode))
        return TP_FILE_MISSING;

    errno = 0;
    f = fopen(path, "rb");
    if (!f) {
        if (out_errno) *out_errno = errno;
        return errno == ENOENT ? TP_FILE_MISSING : TP_FILE_UNREADABLE;
    }

    if (st.st_size == 0) {
        fclose(f);
        return TP_FILE_OK;
    }

    errno = 0;
    n = fread(&byte, 1, 1, f);
    if (n != 1) {
        int e = ferror(f) ? (errno ? errno : EIO) : 0;
        fclose(f);
        if (out_errno) *out_errno = e;
        return TP_FILE_UNREADABLE;
    }
    fclose(f);
    return TP_FILE_OK;
}

uint64_t tp_file_size(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0)
        return 0;
    return (uint64_t)st.st_size;
}

int tp_strcasecmp(const char *a, const char *b)
{
    unsigned char ca, cb;
    if (!a)
        a = "";
    if (!b)
        b = "";
    for (;;) {
        ca = (unsigned char)tolower((unsigned char)*a++);
        cb = (unsigned char)tolower((unsigned char)*b++);
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
}

char *tp_utf8_sanitize(const char *s)
{
    size_t i = 0, o = 0, n;
    char *out;
    if (!s)
        return tp_strdup("");
    n = strlen(s);
    out = (char *)malloc(n * 3 + 1);
    if (!out)
        return NULL;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out[o++] = (char)c;
            i++;
        } else if ((c & 0xE0) == 0xC0 && (unsigned char)s[i + 1] >= 0x80 &&
                   (unsigned char)s[i + 1] < 0xC0) {
            out[o++] = s[i];
            out[o++] = s[i + 1];
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && (unsigned char)s[i + 1] >= 0x80 &&
                   (unsigned char)s[i + 2] >= 0x80) {
            out[o++] = s[i];
            out[o++] = s[i + 1];
            out[o++] = s[i + 2];
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && (unsigned char)s[i + 1] >= 0x80 &&
                   (unsigned char)s[i + 2] >= 0x80 &&
                   (unsigned char)s[i + 3] >= 0x80) {
            out[o++] = s[i];
            out[o++] = s[i + 1];
            out[o++] = s[i + 2];
            out[o++] = s[i + 3];
            i += 4;
        } else {
            /* U+FFFD */
            out[o++] = (char)0xEF;
            out[o++] = (char)0xBF;
            out[o++] = (char)0xBD;
            i++;
        }
    }
    out[o] = '\0';
    (void)strip_slash;
    return out;
}

size_t tp_json_escape(char *buf, size_t cap, const char *s)
{
    size_t need = 0, i;
    if (!s)
        s = "";
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[7];
        size_t el = 1;
        if (c == '"' || c == '\\') {
            u[0] = '\\';
            u[1] = (char)c;
            esc = u;
            el = 2;
        } else if (c == '\n') {
            esc = "\\n";
            el = 2;
        } else if (c == '\r') {
            esc = "\\r";
            el = 2;
        } else if (c == '\t') {
            esc = "\\t";
            el = 2;
        } else if (c < 0x20) {
            snprintf(u, sizeof(u), "\\u%04x", c);
            esc = u;
            el = 6;
        } else {
            esc = (const char *)&c;
            el = 1;
        }
        if (buf && need + el < cap)
            memcpy(buf + need, esc, el);
        need += el;
    }
    if (buf && need < cap)
        buf[need] = '\0';
    else if (buf && cap)
        buf[cap - 1] = '\0';
    return need;
}

static int json_ensure(size_t *off, size_t cap, size_t add)
{
    return (*off + add < cap) ? 0 : -1;
}

int tp_json_write_string(char *buf, size_t cap, size_t *off, const char *s)
{
    size_t n;
    if (json_ensure(off, cap, 1) < 0)
        return -1;
    buf[(*off)++] = '"';
    n = tp_json_escape(buf + *off, cap - *off, s);
    if (*off + n >= cap)
        return -1;
    *off += n;
    if (json_ensure(off, cap, 1) < 0)
        return -1;
    buf[(*off)++] = '"';
    buf[*off] = '\0';
    return 0;
}

int tp_json_write_u64(char *buf, size_t cap, size_t *off, uint64_t v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    if (n < 0 || json_ensure(off, cap, (size_t)n) < 0)
        return -1;
    memcpy(buf + *off, tmp, (size_t)n);
    *off += (size_t)n;
    buf[*off] = '\0';
    return 0;
}

int tp_json_write_u32(char *buf, size_t cap, size_t *off, uint32_t v)
{
    return tp_json_write_u64(buf, cap, off, v);
}

int tp_json_write_bool(char *buf, size_t cap, size_t *off, int v)
{
    const char *s = v ? "true" : "false";
    size_t n = strlen(s);
    if (json_ensure(off, cap, n) < 0)
        return -1;
    memcpy(buf + *off, s, n);
    *off += n;
    buf[*off] = '\0';
    return 0;
}

static int cmp_title(const void *a, const void *b)
{
    const struct tp_track *ta = (const struct tp_track *)a;
    const struct tp_track *tb = (const struct tp_track *)b;
    const char *sa = ta->title ? ta->title : "";
    const char *sb = tb->title ? tb->title : "";
    return tp_strcasecmp(sa, sb);
}

static int cmp_artist_album(const void *a, const void *b)
{
    const struct tp_track *ta = (const struct tp_track *)a;
    const struct tp_track *tb = (const struct tp_track *)b;
    int c = tp_strcasecmp(ta->artist ? ta->artist : "", tb->artist ? tb->artist : "");
    if (c)
        return c;
    c = tp_strcasecmp(ta->album ? ta->album : "", tb->album ? tb->album : "");
    if (c)
        return c;
    if (ta->disc_number != tb->disc_number)
        return (ta->disc_number < tb->disc_number) ? -1 : 1;
    if (ta->track_number != tb->track_number)
        return (ta->track_number < tb->track_number) ? -1 : 1;
    return tp_strcasecmp(ta->title ? ta->title : "", tb->title ? tb->title : "");
}

void tp_sort_tracks_by_title(struct tp_track *tracks, size_t n)
{
    if (tracks && n)
        qsort(tracks, n, sizeof(*tracks), cmp_title);
}

static int cmp_artist_entry(const void *x, const void *y)
{
    const struct tp_artist_entry *a = x, *b = y;
    return tp_strcasecmp(a->name ? a->name : "", b->name ? b->name : "");
}

/* By album title first, because that is what the album list shows on its top
   line and therefore what somebody scanning it is reading. The artist breaks
   ties, so two records with the same title stay next to each other in a
   predictable order rather than in load order. */
static int cmp_album_entry(const void *x, const void *y)
{
    const struct tp_album_entry *a = x, *b = y;
    int c = tp_strcasecmp(a->album ? a->album : "", b->album ? b->album : "");
    return c ? c : tp_strcasecmp(a->artist ? a->artist : "",
                                 b->artist ? b->artist : "");
}

void tp_sort_artists(struct tp_artist_entry *a, size_t n)
{
    if (a && n > 1) qsort(a, n, sizeof *a, cmp_artist_entry);
}

void tp_sort_albums(struct tp_album_entry *a, size_t n)
{
    if (a && n > 1) qsort(a, n, sizeof *a, cmp_album_entry);
}

void tp_sort_tracks_artist_album(struct tp_track *tracks, size_t n)
{
    if (tracks && n)
        qsort(tracks, n, sizeof(*tracks), cmp_artist_album);
}

uint64_t tp_time_ms_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
