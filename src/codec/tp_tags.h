/*
 * What a file says about itself, for the screen.
 *
 * The folder browser has only ever had a file name to show. The one piece of
 * tag handling this app had - skip_id3() in tp_decode.c - exists to find the
 * first MP3 frame and deliberately reads nothing on the way past, so a track
 * played out of a folder shows up as "03 Track.mp3" while the same track
 * reached through the iTunes database shows up with its artist and album.
 *
 * Nothing here reports an error, because there is nothing the caller could do
 * with one: an untagged file and an unreadable one both mean "show the file
 * name", and playback is never held up either way.
 */
#ifndef TP_TAGS_H
#define TP_TAGS_H
#include <stddef.h>
struct tp_tags {
    char title[128];
    char artist[128];
    char album[128];
    int  track;      /* 0 when unknown */
};
/*
 * Read what this file says about itself. Returns 1 if anything at all was
 * found, 0 if not. Never fails destructively: unknown fields are left as
 * empty strings and the caller falls back to the file name.
 */
int tp_tags_read(const char *path, struct tp_tags *out);
#endif
