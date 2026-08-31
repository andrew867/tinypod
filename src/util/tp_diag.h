/*
 * tp_diag.h — leave a trail, so a failure on the device is answerable.
 *
 * TinyPod runs on a device with no debugger attached, started by a launcher
 * that only sees an exit code, printing to a tty nobody is watching. When it
 * dies, what is left is "it did not open". That is not enough to fix
 * anything, and it is not enough to tell a crash from a refusal.
 *
 * So: the current stage is written to a file as it changes, and a fatal signal
 * writes the stage it died in before the process goes. n31-autostart appends
 * the last line to its own log when an app exits badly, and the launcher shows
 * that line on the home screen - so a segfault halfway through reading the
 * database arrives on the glass as the stage it happened in, without anybody
 * having to attach anything.
 *
 * Deliberately a file and not a pipe: the point is that it survives the
 * process, including a process that was killed rather than one that returned.
 */

#ifndef TINYPOD_DIAG_H
#define TINYPOD_DIAG_H

/* Where the trail is written. Overridable for tests; /tmp is a tmpfs on the
   device and is the only reliably writable place there. */
#define TP_DIAG_PATH_DEFAULT "/tmp/tinypod-stage.log"

/* Install the fatal-signal handlers and start a fresh trail. Safe to call
   more than once; safe to fail, in which case nothing is recorded and the app
   behaves exactly as it did before. */
void tp_diag_begin(void);

/* Record what is being attempted now. Cheap: one short write. */
void tp_diag_stage(const char *what);

/* The last stage recorded, for an app that wants to show it. Empty when
   nothing has been recorded. */
const char *tp_diag_last(void);

#endif /* TINYPOD_DIAG_H */
