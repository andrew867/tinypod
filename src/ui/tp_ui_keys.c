#include "tp_ui_keys.h"

#include <stdio.h>

enum tp_key tp_ui_read_key(void)
{
    int c = getchar();
    if (c == EOF || c == 'q')
        return TP_KEY_QUIT;
    if (c == 'j' || c == '+' || c == 's')
        return TP_KEY_DOWN;
    if (c == 'k' || c == '-' || c == 'w')
        return TP_KEY_UP;
    if (c == '\n' || c == ' ')
        return TP_KEY_SELECT;
    if (c == 27)
        return TP_KEY_BACK;
    return TP_KEY_NONE;
}
