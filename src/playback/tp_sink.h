/*
 * PCM output. One thin interface so the player does not care whether it is
 * feeding the N31's ALSA device or a WAV file on a development host.
 */
#ifndef TP_SINK_H
#define TP_SINK_H

#include <stddef.h>
#include <stdint.h>

struct tp_sink;

/*
 * Open the device for 16-bit interleaved PCM at this rate and channel count.
 * NULL on failure, with a user-facing reason in err.
 */
struct tp_sink *tp_sink_open(int rate, int channels, char *err, size_t errsz);

/* Write frames x channels samples. Returns 0, or -1 on a device error. */
int tp_sink_write(struct tp_sink *s, const int16_t *pcm, int samples);

/* Block until queued audio has been played out. */
void tp_sink_drain(struct tp_sink *s);

/*
 * How many times the stream has had to be restarted - each one an underrun,
 * and on this device a 60 ms silence while the codec settles its rate change.
 * tinyalsa recovers from these on its own and reports nothing, so without a
 * count a stream that stutters continuously looks exactly like one that does
 * not.
 */
unsigned long tp_sink_restarts(const struct tp_sink *s);

/* Short description for the Settings screen: "alsa 320 ms". */
int tp_sink_describe(const struct tp_sink *s, char *out, size_t cap);

/*
 * The sink's rate/channel conversion, exposed for tests.
 *
 * It only runs on a device whose codec refuses the track's rate - tinyalsa
 * converts nothing, unlike alsa-lib's plughw - which is not a situation a
 * host build can arrange, so without this the resampler would ship having
 * never run.
 *
 * A converter rather than a one-shot call: both engines carry filter state
 * across blocks, and soxr returns nothing at all until its filter has filled.
 * Close it with tp_sink_close. Returns frames written to out, or -1.
 */
struct tp_sink *tp_sink_convert_open(int src_rate, int dst_rate,
                                     int src_ch, int dst_ch);
int tp_sink_convert_block(struct tp_sink *s, const int16_t *in, int in_frames,
                          int16_t *out, int out_cap_frames);

/*
 * Stop and restart the stream around a pause. Without this the device simply
 * underruns while nothing is being written, and the next write has to recover
 * from a broken stream instead of resuming a stopped one.
 */
void tp_sink_pause(struct tp_sink *s, int paused);

/*
 * Throw away everything queued but not yet heard, and be ready for new
 * samples immediately.
 *
 * For seeking. The device holds about a third of a second, so without this a
 * jump plays the old position for that long before the new one arrives, which
 * sounds like the button was late rather than like a seek.
 *
 * The resampler is reset too: it carries filter state across calls, and
 * feeding it a discontinuity without resetting rings it.
 */
void tp_sink_flush(struct tp_sink *s);

void tp_sink_close(struct tp_sink *s);

/*
 * Where the next sink opens: an alsa-lib device name, or NULL for the card and
 * device numbers.
 *
 * Takes effect at the next open rather than immediately - a PCM's destination
 * is fixed when it is opened, so moving the audio means a new stream. The
 * player closes its cached one when this changes.
 *
 * The names come from /etc/asound.conf on the device and nothing here knows
 * what they mean; the routing is the machine's business.
 */
void tp_sink_set_device(const char *pcm);
const char *tp_sink_get_device(void);

/* Whether this build can output audio at all. */
int tp_sink_available(void);

/* WAV-file sink, for host verification and capture. Same write/close calls. */
struct tp_sink *tp_sink_open_wav(const char *path, int rate, int channels,
                                 char *err, size_t errsz);

#endif
