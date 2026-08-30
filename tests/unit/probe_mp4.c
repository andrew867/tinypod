/*
 * probe_mp4.c — open every file given and say what the MP4 parser makes of it.
 *
 * A triage tool, not a test. Point it at a real library and it tells you which
 * files parse, which do not, and what the parser thinks it found, which is the
 * fastest way to turn "MP4 tables are missing" into a specific file and a
 * specific reason.
 *
 *     probe_mp4 /path/to/*.m4a
 */

#include <stdio.h>
#include <string.h>

#include "../../src/codec/tp_mp4.h"

int main(int argc, char **argv)
{
    int ok = 0, bad = 0;

    for (int i = 1; i < argc; i++) {
        char err[256];
        err[0] = 0;

        struct tp_mp4 *m = tp_mp4_open(argv[i], err, sizeof err);
        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];

        if (!m) {
            bad++;
            printf("FAIL %-14s %s\n", base, err[0] ? err : "(no reason given)");
            continue;
        }

        ok++;
        printf("ok   %-14s %-6s %5d Hz %d ch  %6u frames\n", base,
               tp_mp4_codec(m), tp_mp4_sample_rate(m), tp_mp4_channels(m),
               tp_mp4_frame_count(m));
        tp_mp4_close(m);
    }

    printf("\n%d parsed, %d failed\n", ok, bad);
    return bad ? 1 : 0;
}
