/*
 * tp_volume.h — the one control this device has that nothing could reach.
 *
 * TinyPod had no volume control at all: the only way to change it was tinymix
 * over ssh, which is not a thing anyone does while listening to music. The
 * codec's own control is what the hardware volume is, so this drives that
 * rather than scaling samples on the way past - attenuating in software
 * throws away bits that the codec would have kept, and the codec's control is
 * the one the headphone amplifier actually follows.
 *
 * Through the ALSA mixer, which is the interface for this: the control is
 * "Headphones Playback Volume" on card 0, and tinyalsa's mixer is already
 * linked in for the PCM side. Not sysfs pokes, not register writes.
 *
 * Everything here is safe to call when there is no mixer - a build on a host
 * with no such card, or a device whose codec did not probe. It reports
 * unavailable and does nothing, and the UI hides the row.
 */

#ifndef TP_VOLUME_H
#define TP_VOLUME_H

/*
 * Find the mixer and the playback control. Returns 0 if there is one to
 * drive. Safe to call more than once; the second call is free.
 */
int tp_volume_open(void);
void tp_volume_close(void);

/* Whether there is a control at all. */
int tp_volume_available(void);

/* 0..100, or -1 when there is nothing to ask. */
int tp_volume_get(void);

/* Set to 0..100, clamped. Returns what it ended up at, or -1. */
int tp_volume_set(int percent);

/*
 * Move by `delta` percent, clamped to 0..100. Returns the new value.
 *
 * The step is the caller's, not this module's: holding a volume key
 * auto-repeats, and how fast that should run is a question about the UI.
 */
int tp_volume_step(int delta);

/* The control being driven, for the settings screen to name. */
const char *tp_volume_control_name(void);

#endif /* TP_VOLUME_H */
