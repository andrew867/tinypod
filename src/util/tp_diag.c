/*
 * tp_diag.c — see tp_diag.h.
 *
 * The signal half of this runs in a signal handler, so it uses only
 * async-signal-safe calls: write(2) to an already-open descriptor, and no
 * allocation, no stdio, no snprintf. Everything it needs is prepared in
 * advance for exactly that reason.
 */

#include "tp_diag.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int  s_fd = -1;
static char s_last[96];

static void write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;         /* nothing useful to do about it here */
        p += w;
        n -= (size_t)w;
    }
}

/*
 * A fatal signal. Say which, and what was being attempted, then let the
 * default action produce the exit status the parent will see.
 *
 * Re-raising rather than _exit() so the launcher sees "killed by signal 11"
 * and not "exited 1" - those are different faults and the launcher already
 * distinguishes them.
 */
static void on_fatal(int sig)
{
    static const char pre[] = "\nDIED in: ";
    const char *name;

    switch (sig) {
    case SIGSEGV: name = " (SIGSEGV)\n"; break;
    case SIGBUS:  name = " (SIGBUS)\n";  break;
    case SIGILL:  name = " (SIGILL)\n";  break;
    case SIGFPE:  name = " (SIGFPE)\n";  break;
    case SIGABRT: name = " (SIGABRT)\n"; break;
    default:      name = " (signal)\n";  break;
    }

    if (s_fd >= 0) {
        write_all(s_fd, pre, sizeof pre - 1);
        write_all(s_fd, s_last, strlen(s_last));
        write_all(s_fd, name, strlen(name));
    }
    /* Also to stderr, for the case where somebody IS watching. */
    write_all(2, pre, sizeof pre - 1);
    write_all(2, s_last, strlen(s_last));
    write_all(2, name, strlen(name));

    signal(sig, SIG_DFL);
    raise(sig);
}

void tp_diag_begin(void)
{
    const char *path = getenv("TINYPOD_STAGE_LOG");
    if (!path || !*path) path = TP_DIAG_PATH_DEFAULT;

    if (s_fd >= 0) close(s_fd);
    /* Truncated, not appended: the interesting run is this one, and a file
       that grows without bound on a tmpfs is its own problem. */
    s_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    static const int FATAL[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (unsigned i = 0; i < sizeof FATAL / sizeof FATAL[0]; i++)
        signal(FATAL[i], on_fatal);

    tp_diag_stage("starting");
}

void tp_diag_stage(const char *what)
{
    if (!what) what = "";

    /* Kept for the signal handler, which cannot format anything itself. */
    size_t n = strlen(what);
    if (n >= sizeof s_last) n = sizeof s_last - 1;
    memcpy(s_last, what, n);
    s_last[n] = 0;

    if (s_fd < 0) return;
    write_all(s_fd, "stage: ", 7);
    write_all(s_fd, s_last, n);
    write_all(s_fd, "\n", 1);
}

const char *tp_diag_last(void)
{
    return s_last;
}
