/*
 * tp_build.h — which build is this, actually.
 *
 * It exists because of one specific, expensive confusion: a copy on the
 * device's disk that is months old looks exactly like the one just compiled,
 * right up until an afternoon goes into reproducing a fault that was fixed in
 * April. So TinyPod says on screen and on the command line when it was built
 * and from which commit, and a mismatch takes one look to spot.
 *
 * Baked in with -D by the makefile, into this one small translation unit, so
 * a fresh stamp costs one recompile rather than a rebuild of the world.
 */

#ifndef TINYPOD_BUILD_H
#define TINYPOD_BUILD_H

/* "20260830.1448 g1a2b3c4" - UTC, sortable, and the commit it came from. */
const char *tp_build_version(void);

#endif /* TINYPOD_BUILD_H */
