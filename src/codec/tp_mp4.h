/*
 * Minimal read-only MP4/M4A demuxer: enough of ISO-BMFF to hand raw AAC
 * frames to a decoder, and nothing more.
 *
 * An iTunes-synced .m4a holds AAC frames with no ADTS headers, so the decoder
 * has to be configured from the AudioSpecificConfig in stsd/mp4a/esds, and the
 * frames located through the sample tables (stsz/stsc/stco).
 *
 * Frames are walked with a cursor rather than a precomputed offset table:
 * only the sizes (stsz) and chunk offsets (stco) are held, which keeps an
 * hour-long audiobook to a few hundred KB instead of tens of MB.
 */
#ifndef TP_MP4_H
#define TP_MP4_H

#include <stddef.h>
#include <stdint.h>

struct tp_mp4;

/* NULL on failure; err (if given) gets a one-line reason. */
struct tp_mp4 *tp_mp4_open(const char *path, char *err, size_t errsz);
void tp_mp4_close(struct tp_mp4 *m);

/* AudioSpecificConfig from esds. NULL if the track had none. */
const unsigned char *tp_mp4_asc(const struct tp_mp4 *m, size_t *len);

int tp_mp4_is_aac(const struct tp_mp4 *m);
/* Codec fourcc from stsd, e.g. "mp4a" or "alac". */
const char *tp_mp4_codec(const struct tp_mp4 *m);
int tp_mp4_channels(const struct tp_mp4 *m);
int tp_mp4_sample_rate(const struct tp_mp4 *m);
uint32_t tp_mp4_frame_count(const struct tp_mp4 *m);
uint64_t tp_mp4_duration_ms(const struct tp_mp4 *m);

/*
 * Copy the next frame into buf. Returns its length, 0 at end of track, or -1
 * on a read error or a frame too large for buf.
 */
int tp_mp4_next_frame(struct tp_mp4 *m, unsigned char *buf, size_t bufsz);

/* Restart from the first frame. */
void tp_mp4_rewind(struct tp_mp4 *m);

#endif
