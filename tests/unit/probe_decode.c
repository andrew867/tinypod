/*
 * probe_decode.c — decode every file given and say what actually came out.
 *
 * The MP4 parser reporting a healthy sample table says nothing about whether
 * the fixed-point decoder can turn those samples into audio. This runs the
 * whole path and reports duration, peak and how much of it was silence, which
 * is what separates "played" from "opened and produced nothing".
 *
 *     probe_decode /path/to/*.m4a
 *     probe_decode --wav out.wav /path/to/one.mp3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/codec/tp_decode.h"

static void put_le32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v);         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);   p[3] = (unsigned char)(v >> 24);
}

static void put_le16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v);         p[1] = (unsigned char)(v >> 8);
}

/* Write what was decoded, so it can be listened to rather than believed. */
static int write_wav(const char *path, const short *pcm, long samples,
                     int rate, int ch)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    unsigned data = (unsigned)(samples * 2);
    unsigned char h[44];
    memcpy(h, "RIFF", 4);       put_le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);       put_le16(h + 20, 1);
    put_le16(h + 22, (unsigned)ch);
    put_le32(h + 24, (unsigned)rate);
    put_le32(h + 28, (unsigned)(rate * ch * 2));
    put_le16(h + 32, (unsigned)(ch * 2));
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);  put_le32(h + 40, data);

    fwrite(h, 1, sizeof h, f);
    fwrite(pcm, 2, (size_t)samples, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *wav = NULL;
    int arg = 1;

    if (argc > 2 && !strcmp(argv[1], "--wav")) {
        wav = argv[2];
        arg = 3;
    }

    int ok = 0, bad = 0, silent = 0;

    for (int i = arg; i < argc; i++) {
        char err[256];
        err[0] = 0;

        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];

        struct tp_dec *d = tp_dec_open(argv[i], err, sizeof err);
        if (!d) {
            bad++;
            printf("FAIL  %-14s open: %s\n", base,
                   err[0] ? err : "(no reason given)");
            continue;
        }

        int rate = tp_dec_rate(d);
        int ch = tp_dec_channels(d);

        /* Enough for a few minutes; anything longer is truncated, which is
           fine because this is measuring whether it decodes, not archiving. */
        const long CAP = 60L * 48000 * 2;
        short *pcm = wav ? malloc((size_t)CAP * sizeof(short)) : NULL;

        short buf[TP_DEC_MAX_BLOCK];
        long total = 0;
        int peak = 0;
        for (;;) {
            int n = tp_dec_read(d, buf);
            if (n <= 0)
                break;
            for (int k = 0; k < n; k++) {
                int v = buf[k] < 0 ? -buf[k] : buf[k];
                if (v > peak)
                    peak = v;
            }
            if (pcm && total + n <= CAP) {
                memcpy(pcm + total, buf, (size_t)n * sizeof(short));
            }
            total += n;
            if (total > 200L * 48000 * 2)
                break;                    /* a decoder that never finishes */
        }

        double secs = (rate && ch) ? (double)total / (rate * ch) : 0.0;
        int quiet = peak < 512;
        if (quiet)
            silent++;

        printf("%-5s %-14s %5d Hz %d ch  %6.1f s  peak %5d%s\n",
               quiet ? "QUIET" : "ok", base, rate, ch, secs, peak,
               quiet ? "   <- decoded but silent" : "");

        if (pcm) {
            long n = total < CAP ? total : CAP;
            if (write_wav(wav, pcm, n, rate, ch) == 0)
                printf("      wrote %s (%ld samples)\n", wav, n);
            free(pcm);
        }

        ok++;
        tp_dec_close(d);
    }

    printf("\n%d decoded, %d failed to open, %d silent\n", ok, bad, silent);
    return (bad || silent) ? 1 : 0;
}
