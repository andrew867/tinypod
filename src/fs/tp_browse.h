/*
 * Walking the disk, for the times the library is not the answer.
 *
 * The iTunes database only knows about what iTunes put there. Anything copied
 * on by hand - a folder of FLAC, a podcast, an album that never went near
 * iTunes - is invisible to every other screen in this app while sitting in
 * plain view on the volume. This is how you get at it.
 *
 * One directory at a time, read on demand. Nothing is cached and nothing is
 * indexed: a drill-down only ever needs the level it is showing, and a
 * read-only vfat mount on NAND is not somewhere to go walking recursively.
 */
#ifndef TP_BROWSE_H
#define TP_BROWSE_H

#include <stddef.h>

/* Long enough for the deepest path a vfat volume can hold without being the
   thing that decides how deep you may go. */
#define TP_BROWSE_PATH_MAX 1024
#define TP_BROWSE_NAME_MAX 256

/*
 * More than this in one directory and the rest are not listed.
 *
 * It is a bound on memory rather than a judgement about how many files
 * someone may keep: at this size the list is already longer than anyone will
 * scroll, and the alternative is an allocation decided by whatever is on the
 * disk. When it bites, the caller is told rather than quietly shown a
 * shorter directory than the one that is there.
 */
#define TP_BROWSE_MAX 4096

struct tp_browse_entry {
    char name[TP_BROWSE_NAME_MAX];
    int  is_dir;
    int  playable;           /* a file this build can decode */
};

struct tp_browse {
    char                    path[TP_BROWSE_PATH_MAX];
    struct tp_browse_entry *entries;
    size_t                  count;
    int                     truncated;   /* there were more than we listed */
};

/*
 * Read one directory. Directories first, then playable files, each sorted by
 * name without regard to case - which is the order a person expects and not
 * the order readdir gives, that being whatever the filesystem felt like.
 *
 * Files this build cannot decode are left out: a browser that lists them
 * only offers a choice that ends in an error. Entries beginning with a dot
 * are left out too unless show_hidden.
 *
 * Returns 0, or -1 with the reason in err.
 */
int tp_browse_open(struct tp_browse *b, const char *path, int show_hidden,
                   char *err, size_t errsz);

void tp_browse_free(struct tp_browse *b);

/* Whether this build can decode a file with this name, by its extension. */
int tp_browse_playable(const char *name);

/*
 * Join a directory and an entry into a path, the way this browser means it.
 * Returns 0, or -1 if the result would not fit.
 */
int tp_browse_join(char *out, size_t cap, const char *dir, const char *name);

/* The parent of a path, or -1 at the root. */
int tp_browse_parent(char *out, size_t cap, const char *path);

#endif /* TP_BROWSE_H */
