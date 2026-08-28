/*
 * tp_lv_input.h — three buttons, and what they have to cover.
 *
 * The device has volume up, volume down, play/pause and home down one side, and
 * no touchscreen. HOME is not available to this app: the N31 launcher watches
 * for it and terminates whatever is running, which is how you get back to the
 * launcher from any app. Taking it here would break that, so HOME is left
 * alone and TinyPod has three buttons, not four.
 *
 * Three buttons have to cover navigating a library AND transport controls, so
 * PLAY is doubled up by duration:
 *
 *   VOL +/-      move the selection; on Now Playing, previous/next track
 *   PLAY short   select, or play/pause on Now Playing
 *   PLAY long    back, one level at a time
 *   HOME         exits to the launcher (not seen here)
 *
 * Long-press is on PLAY rather than on a volume key because holding a volume
 * key is how you scroll a long list, and the two would fight.
 */

#ifndef TP_LV_INPUT_H
#define TP_LV_INPUT_H

enum tp_lv_key {
    TP_LV_NONE = 0,
    TP_LV_UP,
    TP_LV_DOWN,
    TP_LV_SELECT,     /* PLAY, released before the long-press threshold */
    TP_LV_BACK,       /* PLAY, held past it */
    TP_LV_QUIT
};

/* Open the button devices. Returns how many were found; zero means nothing can
   be driven, which the caller should say out loud rather than sit silently. */
int tp_lv_input_open(void);
void tp_lv_input_close(void);

/*
 * The next event, or TP_LV_NONE. Never blocks.
 *
 * A long press has to fire while the button is still down - waiting for the
 * release would mean nothing happens until you let go, which feels broken - so
 * this must be called regularly even when there is no input to read.
 */
enum tp_lv_key tp_lv_input_poll(void);

/* True while PLAY is held and the long-press has not yet fired, so the UI can
   show that something is about to happen. */
int tp_lv_input_holding(void);

#endif
