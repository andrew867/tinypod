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
 * Stop and restart the stream around a pause. Without this the device simply
 * underruns while nothing is being written, and the next write has to recover
 * from a broken stream instead of resuming a stopped one.
 */
void tp_sink_pause(struct tp_sink *s, int paused);

void tp_sink_close(struct tp_sink *s);

/* Whether this build can output audio at all. */
int tp_sink_available(void);

/* WAV-file sink, for host verification and capture. Same write/close calls. */
struct tp_sink *tp_sink_open_wav(const char *path, int rate, int channels,
                                 char *err, size_t errsz);

#endif
