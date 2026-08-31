/*
 * tp_dec_ff.h — the FFmpeg decoder, behind the same shape as the Helix one.
 *
 * Present only when built with TP_WITH_FFMPEG. It is the fallback, not the
 * default: AAC and MP3 keep going through the Helix fixed-point decoders,
 * which are proven on this device, allocate little, and are what the library
 * an iPod actually holds is made of. FFmpeg is what makes everything ELSE
 * play - FLAC, Vorbis, Opus, ALAC, WMA, Musepack, AC3 and the rest - without
 * changing the path that already works.
 *
 * Output is always interleaved 16-bit PCM at the stream's own rate, downmixed
 * to at most two channels, because that is what the sink takes and what the
 * rest of TinyPod is written against.
 */

#ifndef TP_DEC_FF_H
#define TP_DEC_FF_H

#include <stddef.h>
#include <stdint.h>

struct tp_ff;

/* NULL if the file cannot be opened or holds no audio this build can decode.
   `err` (if given) gets a one-line, user-facing reason. */
struct tp_ff *tp_ff_open(const char *path, char *err, size_t errsz);
void tp_ff_close(struct tp_ff *f);

int tp_ff_rate(const struct tp_ff *f);
int tp_ff_channels(const struct tp_ff *f);
uint64_t tp_ff_duration_ms(const struct tp_ff *f);
const char *tp_ff_codec_name(const struct tp_ff *f);

/* Next block of interleaved 16-bit PCM: samples written (frames x channels),
   0 at end of stream, -1 on an unrecoverable error. `max` is the room in
   `out`, in samples. */
int tp_ff_read(struct tp_ff *f, int16_t *out, int max);

#endif /* TP_DEC_FF_H */
