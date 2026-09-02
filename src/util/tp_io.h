/*
 * Where an I/O failure happened, kept instead of thrown away.
 *
 * Every reader in this app used to answer failure with a bare -1. Five
 * distinct faults inside one function - open, seek, too short, out of memory,
 * short read - all came out as the same number, and by the time it reached
 * the screen it was one sentence naming a directory. On a device whose flash
 * translation layer returns EIO for some blocks and ENOENT for others, "could
 * not load the library" is not a diagnosis; it is the absence of one.
 *
 * So a failure carries what it knew at the time: which reader, what it was
 * doing, which file, where in it, how much it wanted and got, and errno. It
 * lives in struct tp_library because that is the thing being loaded - no
 * globals, and no threading a parameter through five call sites that do not
 * otherwise care.
 */
#ifndef TP_IO_H
#define TP_IO_H

#include <stddef.h>

#define TP_IO_READER_MAX 16
#define TP_IO_WHAT_MAX   24
#define TP_IO_PATH_MAX   256

struct tp_io_err {
    char        reader[TP_IO_READER_MAX];  /* "sqlite-itdb", "raw-scan", ... */
    char        what[TP_IO_WHAT_MAX];      /* "open", "read", "parse", ... */
    char        path[TP_IO_PATH_MAX];
    long long   off;                       /* -1 when it does not apply */
    long long   want;
    long long   got;
    int         err;                       /* errno at the point of failure */
    int         set;
};

void tp_io_err_clear(struct tp_io_err *e);

/*
 * Record a failure. The first one wins: a reader that fails at its third
 * step has usually already told you the interesting thing, and later
 * failures are consequences.
 */
void tp_io_err_set(struct tp_io_err *e, const char *reader, const char *what,
                   const char *path, long long off, long long want,
                   long long got, int err);

/*
 * One line, for a screen that has one line. Distinguishes a missing file from
 * an unreadable one, because those want completely different responses and
 * both used to read as "not found".
 */
int tp_io_err_format(const struct tp_io_err *e, char *out, size_t cap);

#endif /* TP_IO_H */
