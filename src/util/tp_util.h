#ifndef TP_UTIL_H
#define TP_UTIL_H

#include <stddef.h>
#include <stdint.h>

struct tp_track;

char *tp_strdup(const char *s);
char *tp_strndup(const char *s, size_t n);
void *tp_xmalloc(size_t n);
void *tp_xcalloc(size_t n, size_t sz);
void *tp_xrealloc(void *p, size_t n);

/* Join path components with '/'. Result must be freed. */
char *tp_path_join2(const char *a, const char *b);
char *tp_path_join3(const char *a, const char *b, const char *c);

int tp_file_exists(const char *path);
int tp_is_dir(const char *path);
int tp_is_readable_file(const char *path);
uint64_t tp_file_size(const char *path);

/* UTF-8: replace invalid sequences with U+FFFD for display. Caller frees. */
char *tp_utf8_sanitize(const char *s);

/* Case-insensitive ASCII compare */
int tp_strcasecmp(const char *a, const char *b);

/* JSON string escape into buf; returns bytes needed excluding NUL. */
size_t tp_json_escape(char *buf, size_t cap, const char *s);
int tp_json_write_string(char *buf, size_t cap, size_t *off, const char *s);
int tp_json_write_u64(char *buf, size_t cap, size_t *off, uint64_t v);
int tp_json_write_u32(char *buf, size_t cap, size_t *off, uint32_t v);
int tp_json_write_bool(char *buf, size_t cap, size_t *off, int v);

/* Sort helpers */
void tp_sort_tracks_by_title(struct tp_track *tracks, size_t n);
void tp_sort_tracks_artist_album(struct tp_track *tracks, size_t n);

uint64_t tp_time_ms_now(void);

#endif
