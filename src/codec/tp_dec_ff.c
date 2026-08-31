/*
 * tp_dec_ff.c — see tp_dec_ff.h.
 *
 * A deliberately small slice of FFmpeg: open, find the audio stream, decode,
 * resample to interleaved 16-bit, hand it out in blocks. No seeking, no
 * filtering, no video. The rest of TinyPod does not know it is here.
 *
 * Two things this has to get right that the Helix path never had to.
 *
 * A decoded frame can be larger than the caller's buffer - some codecs emit
 * four thousand frames at a time, and with two channels that is more samples
 * than TP_DEC_MAX_BLOCK holds - so what does not fit is kept and handed out on
 * the next call. Returning a short read and dropping the rest would lose audio
 * quietly, which is the worst way to lose it.
 *
 * And the output is capped at two channels. The sink is stereo, and a 5.1
 * stream decoded to six channels and written to a stereo device plays at three
 * times the speed with half the content missing.
 */

#include "tp_dec_ff.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

struct tp_ff {
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frame;
    int              stream;

    int      rate;
    int      channels;
    uint64_t duration_ms;
    char     codec[24];

    /* Decoded samples not yet handed to the caller. See the note above: a
       frame can be bigger than the caller's buffer, and the remainder has to
       survive until the next call. */
    int16_t *pending;
    int      pending_cap;      /* samples */
    int      pending_len;      /* samples held */
    int      pending_at;       /* samples already given out */

    int      eof;              /* the demuxer is finished; the decoder may not be */
};

static void fail(char *err, size_t errsz, const char *msg)
{
    if (err && errsz)
        snprintf(err, errsz, "%s", msg);
}

void tp_ff_close(struct tp_ff *f)
{
    if (!f)
        return;
    if (f->swr)   swr_free(&f->swr);
    if (f->frame) av_frame_free(&f->frame);
    if (f->pkt)   av_packet_free(&f->pkt);
    if (f->dec)   avcodec_free_context(&f->dec);
    if (f->fmt)   avformat_close_input(&f->fmt);
    free(f->pending);
    free(f);
}

struct tp_ff *tp_ff_open(const char *path, char *err, size_t errsz)
{
    struct tp_ff *f;
    const AVCodec *codec;
    AVChannelLayout out_layout;
    int rc;

    if (!path) {
        fail(err, errsz, "no path");
        return NULL;
    }

    f = calloc(1, sizeof(*f));
    if (!f) {
        fail(err, errsz, "out of memory");
        return NULL;
    }
    f->stream = -1;

    if (avformat_open_input(&f->fmt, path, NULL, NULL) < 0) {
        fail(err, errsz, "the file could not be opened");
        tp_ff_close(f);
        return NULL;
    }
    if (avformat_find_stream_info(f->fmt, NULL) < 0) {
        fail(err, errsz, "no readable streams in the file");
        tp_ff_close(f);
        return NULL;
    }

    f->stream = av_find_best_stream(f->fmt, AVMEDIA_TYPE_AUDIO, -1, -1,
                                    &codec, 0);
    if (f->stream < 0 || !codec) {
        fail(err, errsz, "no audio this build can decode");
        tp_ff_close(f);
        return NULL;
    }

    f->dec = avcodec_alloc_context3(codec);
    if (!f->dec) {
        fail(err, errsz, "out of memory");
        tp_ff_close(f);
        return NULL;
    }
    if (avcodec_parameters_to_context(f->dec,
                                      f->fmt->streams[f->stream]->codecpar) < 0) {
        fail(err, errsz, "the audio stream could not be set up");
        tp_ff_close(f);
        return NULL;
    }
    /* One thread. The device has one core worth using and a decoder thread
       pool would cost memory this machine does not have to spare. */
    f->dec->thread_count = 1;

    if (avcodec_open2(f->dec, codec, NULL) < 0) {
        fail(err, errsz, "the audio stream could not be decoded");
        tp_ff_close(f);
        return NULL;
    }

    f->rate = f->dec->sample_rate;
    f->channels = f->dec->ch_layout.nb_channels > 2
                      ? 2 : f->dec->ch_layout.nb_channels;
    if (f->channels < 1) f->channels = 1;
    if (f->rate <= 0) {
        fail(err, errsz, "the audio stream has no sample rate");
        tp_ff_close(f);
        return NULL;
    }

    av_channel_layout_default(&out_layout, f->channels);
    rc = swr_alloc_set_opts2(&f->swr, &out_layout, AV_SAMPLE_FMT_S16, f->rate,
                             &f->dec->ch_layout, f->dec->sample_fmt,
                             f->rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (rc < 0 || !f->swr || swr_init(f->swr) < 0) {
        fail(err, errsz, "the audio could not be converted for output");
        tp_ff_close(f);
        return NULL;
    }

    f->pkt = av_packet_alloc();
    f->frame = av_frame_alloc();
    if (!f->pkt || !f->frame) {
        fail(err, errsz, "out of memory");
        tp_ff_close(f);
        return NULL;
    }

    if (f->fmt->duration > 0 && f->fmt->duration != AV_NOPTS_VALUE)
        f->duration_ms = (uint64_t)(f->fmt->duration / (AV_TIME_BASE / 1000));

    snprintf(f->codec, sizeof(f->codec), "%s", codec->name);
    return f;
}

int tp_ff_rate(const struct tp_ff *f)       { return f ? f->rate : 0; }
int tp_ff_channels(const struct tp_ff *f)   { return f ? f->channels : 0; }
uint64_t tp_ff_duration_ms(const struct tp_ff *f)
{
    return f ? f->duration_ms : 0;
}
const char *tp_ff_codec_name(const struct tp_ff *f)
{
    return f ? f->codec : "";
}

/* Room for `samples` in the pending buffer, growing it if need be. */
static int pending_room(struct tp_ff *f, int samples)
{
    if (f->pending_cap >= samples)
        return 0;
    int16_t *p = realloc(f->pending, (size_t)samples * sizeof(int16_t));
    if (!p)
        return -1;
    f->pending = p;
    f->pending_cap = samples;
    return 0;
}

/* Convert one decoded frame into the pending buffer. */
static int take_frame(struct tp_ff *f)
{
    int want = swr_get_out_samples(f->swr, f->frame->nb_samples);
    if (want < f->frame->nb_samples)
        want = f->frame->nb_samples;

    if (pending_room(f, (want + 64) * f->channels) != 0)
        return -1;

    uint8_t *out = (uint8_t *)f->pending;
    int got = swr_convert(f->swr, &out, want,
                          (const uint8_t **)f->frame->extended_data,
                          f->frame->nb_samples);
    if (got < 0)
        return -1;

    f->pending_len = got * f->channels;
    f->pending_at = 0;
    return 0;
}

int tp_ff_read(struct tp_ff *f, int16_t *out, int max)
{
    if (!f || !out || max <= 0)
        return -1;

    for (;;) {
        /* Anything already decoded goes first, a block at a time. */
        if (f->pending_at < f->pending_len) {
            int n = f->pending_len - f->pending_at;
            if (n > max)
                n = max;
            /* Whole frames only, so a block never splits a stereo pair and
               leaves the channels swapped for the rest of the track. */
            n -= n % f->channels;
            if (n <= 0)
                return 0;
            memcpy(out, f->pending + f->pending_at, (size_t)n * sizeof(int16_t));
            f->pending_at += n;
            return n;
        }

        int rc = avcodec_receive_frame(f->dec, f->frame);
        if (rc == 0) {
            if (take_frame(f) != 0)
                return -1;
            continue;
        }
        if (rc == AVERROR_EOF)
            return 0;
        if (rc != AVERROR(EAGAIN))
            return -1;

        if (f->eof) {
            /* Nothing more to send and the decoder wants more: it has been
               flushed and is done. */
            return 0;
        }

        /* Feed it. Packets from other streams are skipped rather than sent -
           a file with cover art in it is a file with a video stream in it. */
        rc = av_read_frame(f->fmt, f->pkt);
        if (rc < 0) {
            f->eof = 1;
            avcodec_send_packet(f->dec, NULL);      /* flush */
            continue;
        }
        if (f->pkt->stream_index != f->stream) {
            av_packet_unref(f->pkt);
            continue;
        }
        rc = avcodec_send_packet(f->dec, f->pkt);
        av_packet_unref(f->pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN))
            return -1;
    }
}
