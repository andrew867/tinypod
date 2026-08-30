/*
 * tp_build.c — see tp_build.h.
 *
 * The defaults are "unknown" rather than a plausible-looking date, because a
 * wrong version is worse than an absent one: an absent one makes you go and
 * look, and a wrong one makes you stop looking.
 */

#include "tp_build.h"

#ifndef TP_BUILD_STAMP
#define TP_BUILD_STAMP "unknown"
#endif

#ifndef TP_BUILD_GIT
#define TP_BUILD_GIT "nogit"
#endif

const char *tp_build_version(void)
{
    return TP_BUILD_STAMP " g" TP_BUILD_GIT;
}
