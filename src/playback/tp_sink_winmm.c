/*
 * tp_sink_winmm.c — PCM out through Windows waveOut, for host verification.
 *
 * There is a WAV sink already, and writing a file proves the decoder produced
 * samples. It does not prove they are the right samples: a channel swap, a
 * byte order mistake or a rate that is off by a factor sounds obviously wrong
 * and looks perfectly fine in a peak measurement. So this exists to actually
 * hear the fixed-point decoders on real files before trusting them on a device
 * that is harder to inspect.
 *
 * waveOut rather than WASAPI on purpose: it is a handful of calls, it needs no
 * COM, and it will happily take 16-bit interleaved PCM at whatever rate the
 * decoder produces. Nothing here ships to the iPod - the device uses ALSA -
 * and none of it is in the N31 build.
 *
 * The ring of buffers is what keeps it gapless. waveOut plays what is queued
 * and calls back as each block completes; writing one block at a time and
 * waiting would leave a hole between every block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <mmsystem.h>

#include "tp_sink.h"

/* Eight blocks of a fifth of a second. Enough that a slow decode does not
   underrun, short enough that stopping is not audibly late. */
#define NBLK   8
#define BLK_MS 200

struct tp_sink {
    HWAVEOUT     h;
    WAVEHDR      hdr[NBLK];
    short       *blk[NBLK];
    int          blk_samples;      /* per block, frames x channels */
    int          fill;             /* samples already in blk[cur] */
    int          cur;
    int          rate, channels;
    HANDLE       done;             /* signalled as each block completes */
    volatile LONG queued;
};

static void CALLBACK on_done(HWAVEOUT h, UINT msg, DWORD_PTR inst,
                             DWORD_PTR p1, DWORD_PTR p2)
{
    struct tp_sink *s = (struct tp_sink *)inst;
    (void)h; (void)p1; (void)p2;
    if (msg == WOM_DONE && s) {
        InterlockedDecrement(&s->queued);
        SetEvent(s->done);
    }
}

static void fail(char *err, size_t errsz, const char *msg)
{
    if (err && errsz)
        snprintf(err, errsz, "%s", msg);
}

struct tp_sink *tp_sink_open(int rate, int channels, char *err, size_t errsz)
{
    WAVEFORMATEX wf;
    struct tp_sink *s;
    int i;

    if (rate <= 0 || channels <= 0 || channels > 2) {
        fail(err, errsz, "unsupported format");
        return NULL;
    }

    s = calloc(1, sizeof(*s));
    if (!s) {
        fail(err, errsz, "out of memory");
        return NULL;
    }

    s->rate = rate;
    s->channels = channels;
    s->blk_samples = (rate * BLK_MS / 1000) * channels;

    s->done = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!s->done) {
        fail(err, errsz, "cannot create event");
        free(s);
        return NULL;
    }

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = (DWORD)(rate * channels * 2);

    if (waveOutOpen(&s->h, WAVE_MAPPER, &wf, (DWORD_PTR)on_done,
                    (DWORD_PTR)s, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        fail(err, errsz, "no audio output device");
        CloseHandle(s->done);
        free(s);
        return NULL;
    }

    for (i = 0; i < NBLK; i++) {
        s->blk[i] = malloc((size_t)s->blk_samples * sizeof(short));
        if (!s->blk[i]) {
            fail(err, errsz, "out of memory");
            tp_sink_close(s);
            return NULL;
        }
    }
    return s;
}

/* Hand the current block to the device and move to the next free one. */
static int flush_block(struct tp_sink *s)
{
    WAVEHDR *h = &s->hdr[s->cur];

    if (s->fill == 0)
        return 0;

    /* Wait for this slot to come back before reusing it. */
    while (h->dwFlags & WHDR_PREPARED) {
        if (h->dwFlags & WHDR_DONE) {
            waveOutUnprepareHeader(s->h, h, sizeof(*h));
            break;
        }
        WaitForSingleObject(s->done, 200);
    }

    memset(h, 0, sizeof(*h));
    h->lpData = (LPSTR)s->blk[s->cur];
    h->dwBufferLength = (DWORD)(s->fill * sizeof(short));

    if (waveOutPrepareHeader(s->h, h, sizeof(*h)) != MMSYSERR_NOERROR)
        return -1;
    InterlockedIncrement(&s->queued);
    if (waveOutWrite(s->h, h, sizeof(*h)) != MMSYSERR_NOERROR) {
        InterlockedDecrement(&s->queued);
        return -1;
    }

    s->cur = (s->cur + 1) % NBLK;
    s->fill = 0;
    return 0;
}

int tp_sink_write(struct tp_sink *s, const int16_t *pcm, int samples)
{
    int off = 0;

    if (!s || !pcm || samples < 0)
        return -1;

    while (off < samples) {
        int room = s->blk_samples - s->fill;
        int n = samples - off;
        if (n > room)
            n = room;

        memcpy(s->blk[s->cur] + s->fill, pcm + off, (size_t)n * sizeof(short));
        s->fill += n;
        off += n;

        if (s->fill == s->blk_samples && flush_block(s) != 0)
            return -1;
    }
    return 0;
}

void tp_sink_drain(struct tp_sink *s)
{
    if (!s)
        return;
    flush_block(s);
    while (s->queued > 0)
        WaitForSingleObject(s->done, 200);
}

void tp_sink_pause(struct tp_sink *s, int paused)
{
    if (!s)
        return;
    if (paused)
        waveOutPause(s->h);
    else
        waveOutRestart(s->h);
}

void tp_sink_close(struct tp_sink *s)
{
    int i;

    if (!s)
        return;

    if (s->h) {
        waveOutReset(s->h);
        for (i = 0; i < NBLK; i++) {
            if (s->hdr[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(s->h, &s->hdr[i], sizeof(s->hdr[i]));
        }
        waveOutClose(s->h);
    }
    for (i = 0; i < NBLK; i++)
        free(s->blk[i]);
    if (s->done)
        CloseHandle(s->done);
    free(s);
}

int tp_sink_available(void)
{
    return waveOutGetNumDevs() > 0;
}
