#ifndef TP_UI_KEYS_H
#define TP_UI_KEYS_H
enum tp_key { TP_KEY_NONE=0, TP_KEY_UP, TP_KEY_DOWN, TP_KEY_SELECT, TP_KEY_BACK, TP_KEY_HOLD, TP_KEY_QUIT };
enum tp_key tp_ui_read_key(void);
#endif
