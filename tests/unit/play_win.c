/*
 * play_win.c — decode a file and play it, on Windows, through waveOut.
 *
 * The point is to hear it. A peak measurement says samples came out; it does
 * not say the channels are the right way round, the rate is right, or that the
 * fixed-point decoder is producing music rather than noise with a plausible
 * amplitude. Those are all obvious in one second of listening and invisible in
 * a test that only counts.
 *
 *     play_win <file> [seconds]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/codec/tp_decode.h"
#include "../../src/playback/tp_sink.h"

int main(int argc, char **argv)
{
    char err[256];
    double limit;

    if (argc < 2) {
        fprintf(stderr, "usage: play_win <file> [seconds]\n");
        return 2;
    }
    limit = argc > 2 ? atof(argv[2]) : 10.0;

    err[0] = 0;
    struct tp_dec *d = tp_dec_open(argv[1], err, sizeof err);
    if (!d) {
        fprintf(stderr, "decode: %s\n", err[0] ? err : "cannot open");
        return 1;
    }

    int rate = tp_dec_rate(d), ch = tp_dec_channels(d);
    printf("%s: %s %d Hz %d ch\n", argv[1], tp_dec_codec_name(d), rate, ch);

    if (!tp_sink_available()) {
        fprintf(stderr, "no audio output device\n");
        tp_dec_close(d);
        return 1;
    }

    err[0] = 0;
    struct tp_sink *s = tp_sink_open(rate, ch, err, sizeof err);
    if (!s) {
        fprintf(stderr, "sink: %s\n", err[0] ? err : "cannot open");
        tp_dec_close(d);
        return 1;
    }

    short buf[TP_DEC_MAX_BLOCK];
    long total = 0;
    long cap = (long)(limit * rate * ch);
    int peak = 0;

    for (;;) {
        int n = tp_dec_read(d, buf);
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++) {
            int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
        }
        if (tp_sink_write(s, buf, n) != 0) {
            fprintf(stderr, "the audio device stopped accepting data\n");
            break;
        }
        total += n;
        if (total >= cap)
            break;
    }

    tp_sink_drain(s);
    printf("played %.1f s, peak %d\n", (double)total / (rate * ch), peak);

    tp_sink_close(s);
    tp_dec_close(d);
    return 0;
}
