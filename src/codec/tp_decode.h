/*
 * One decode interface over the formats an iPod library actually contains.
 *
 * AAC (in MP4/M4A, and raw ADTS) and MP3 go through the Helix fixed-point
 * decoders; WAV is read straight through. Everything is integer maths - the
 * N31 is armv7 soft-float, so a floating-point decoder would spend its time in
 * libgcc rather than decoding.
 *
 * Rate and channel count are known as soon as tp_dec_open returns, because
 * open decodes the first block: with HE-AAC the output rate is not the rate in
 * the container, and the audio sink has to be opened at the real one.
 */
#ifndef TP_DECODE_H
#define TP_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* HE-AAC/SBR doubles the block: 2048 samples per channel, 2 channels. */
#define TP_DEC_MAX_BLOCK 4096

struct tp_dec;

/* NULL on failure; err (if given) gets a one-line, user-facing reason. */
struct tp_dec *tp_dec_open(const char *path, char *err, size_t errsz);
void tp_dec_close(struct tp_dec *d);

int tp_dec_rate(const struct tp_dec *d);
int tp_dec_channels(const struct tp_dec *d);
uint64_t tp_dec_duration_ms(const struct tp_dec *d);
const char *tp_dec_codec_name(const struct tp_dec *d);

/*
 * Next block of interleaved 16-bit PCM. Returns the number of samples written
 * (frames x channels), 0 at end of stream, -1 on an unrecoverable error.
 * out must have room for TP_DEC_MAX_BLOCK samples.
 */
int tp_dec_read(struct tp_dec *d, int16_t *out);

#endif
