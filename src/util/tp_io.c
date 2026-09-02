#include "tp_io.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void tp_io_err_clear(struct tp_io_err *e)
{
    if (e)
        memset(e, 0, sizeof(*e));
}

void tp_io_err_set(struct tp_io_err *e, const char *reader, const char *what,
                   const char *path, long long off, long long want,
                   long long got, int err)
{
    if (!e || e->set)
        return;                 /* the first failure is the informative one */

    snprintf(e->reader, sizeof e->reader, "%s", reader ? reader : "?");
    snprintf(e->what, sizeof e->what, "%s", what ? what : "?");

    /*
     * The tail of a long path, not the head. Everything interesting is at the
     * end - which file, in which folder - and the front is the mount point,
     * which the caller already knows because it passed it in.
     */
    if (path) {
        size_t n = strlen(path);
        if (n < sizeof e->path) {
            memcpy(e->path, path, n + 1);
        } else {
            size_t keep = sizeof e->path - 4;
            memcpy(e->path, "...", 3);
            memcpy(e->path + 3, path + (n - keep), keep + 1);
        }
    }

    e->off = off;
    e->want = want;
    e->got = got;
    e->err = err;
    e->set = 1;
}

int tp_io_err_format(const struct tp_io_err *e, char *out, size_t cap)
{
    const char *why;

    if (!out || !cap)
        return -1;
    out[0] = 0;
    if (!e || !e->set)
        return -1;

    /*
     * Missing and unreadable are different problems. One means the database
     * points at a file that is not there; the other means the storage would
     * not give it up, which on this device is the flash translation layer and
     * not the library at all.
     */
    switch (e->err) {
    case 0:      why = "";                       break;
    case ENOENT: why = " (not there)";           break;
    case EIO:    why = " (I/O error)";           break;
    case EACCES: why = " (permission denied)";   break;
    case ENOMEM: why = " (out of memory)";       break;
    default:     why = " (errno)";               break;
    }

    if (e->want > 0)
        snprintf(out, cap, "%s: %s failed at %lld, wanted %lld got %lld%s: %s",
                 e->reader, e->what, e->off, e->want, e->got, why, e->path);
    else
        snprintf(out, cap, "%s: %s failed%s: %s",
                 e->reader, e->what, why, e->path);
    return 0;
}
