/*
 * The files this volume would not give us.
 *
 * The N31's FAT volume sits behind a reimplemented flash translation layer
 * that still returns I/O errors over some regions, so a track can be present,
 * be the right size, have a perfectly good header, and simply not read. When
 * that happens the player skips it and carries on - the one thing it must
 * never do is stop the queue - and the row is drawn greyed out, the way
 * iTunes marks a file it can no longer find. This is where "which ones" is
 * kept.
 *
 * In memory only, for the life of the process. There is nowhere to write it:
 * the volume is mounted read only, and the whole point of this list is that
 * the volume is the thing that is failing. Nothing here survives a restart,
 * which is the honest behaviour anyway - a fresh run should give every file
 * another go rather than inherit a grudge from a session whose failures may
 * have been a bad mount rather than bad sectors.
 *
 * Bounded: 256 paths, oldest evicted first. A volume failing everywhere would
 * otherwise turn a diagnostic aid into a memory leak on a machine with 55 MiB
 * of RAM.
 *
 * Safe to call from any thread. tp_badfile_mark is reached from the decode
 * thread as well as the UI thread, so the table is guarded by a mutex.
 */
#ifndef TP_BADFILE_H
#define TP_BADFILE_H

#include <stdbool.h>

/*
 * Remember that `path` would not play. Cheap to call repeatedly with the
 * same path; recording one twice does not grow anything.
 */
void tp_badfile_mark(const char *path);

/* Has this path failed before? */
bool tp_badfile_is_bad(const char *path);

/* Forget one, for a retry that succeeded. */
void tp_badfile_clear(const char *path);

/* How many are known bad, for a settings screen to report. */
unsigned tp_badfile_count(void);

/* Forget all of them - the volume may have been remounted. */
void tp_badfile_reset(void);

#endif /* TP_BADFILE_H */
