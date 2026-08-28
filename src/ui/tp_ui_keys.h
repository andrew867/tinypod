#ifndef TP_UI_KEYS_H
#define TP_UI_KEYS_H

/*
 * One key reader for the whole UI.
 *
 * The point of it is the escape sequences. An arrow key is three bytes - ESC,
 * '[', then a letter - so anything reading a single byte sees a bare ESC and
 * treats it as "back", then gets two stray bytes it does not understand. On the
 * home screen that quit the program.
 *
 * A real ESC press has to keep working too, so the two are told apart by
 * waiting a moment after an ESC: bytes that follow immediately are a sequence,
 * and silence means the user pressed Escape.
 */

enum tp_key {
    TP_KEY_NONE = 0,
    TP_KEY_UP,
    TP_KEY_DOWN,
    TP_KEY_LEFT,
    TP_KEY_RIGHT,
    TP_KEY_SELECT,
    TP_KEY_BACK,
    TP_KEY_PLAYPAUSE,
    TP_KEY_NEXT,
    TP_KEY_PREV,
    TP_KEY_STOP,
    TP_KEY_QUIT
};

/*
 * Read one key from `fd`, which must already have data waiting. Returns
 * TP_KEY_NONE for anything unrecognised, and for the middle of a sequence.
 */
enum tp_key tp_ui_read_key(int fd);

#endif
