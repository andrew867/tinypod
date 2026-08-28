#include "tp_ui_keys.h"

#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * Is there another byte right now? Used only to tell a bare Escape from the
 * start of an arrow sequence. The wait is short enough not to be felt and long
 * enough for the rest of a sequence to arrive, which is a local terminal or a
 * pty either way.
 */
static int more_input(int fd, long usec)
{
    fd_set r;
    struct timeval tv;

    FD_ZERO(&r);
    FD_SET(fd, &r);
    tv.tv_sec = 0;
    tv.tv_usec = usec;

    return select(fd + 1, &r, NULL, NULL, &tv) > 0;
}

static int read_byte(int fd, unsigned char *out)
{
    return read(fd, out, 1) == 1;
}

enum tp_key tp_ui_read_key(int fd)
{
    unsigned char c;

    if (!read_byte(fd, &c))
        return TP_KEY_QUIT;          /* stdin closed */

    if (c == 27) {
        unsigned char a, b;

        /* Nothing followed: a real Escape. */
        if (!more_input(fd, 30000))
            return TP_KEY_BACK;
        if (!read_byte(fd, &a))
            return TP_KEY_BACK;

        /* CSI or SS3 - xterm sends the first, some terminals the second in
           application cursor mode, and the final byte is the same either way. */
        if (a != '[' && a != 'O')
            return TP_KEY_NONE;
        if (!read_byte(fd, &b))
            return TP_KEY_NONE;

        switch (b) {
        case 'A': return TP_KEY_UP;
        case 'B': return TP_KEY_DOWN;
        case 'C': return TP_KEY_RIGHT;
        case 'D': return TP_KEY_LEFT;
        default:
            /* Home, End, PgUp and friends are longer and end in a letter or
               a tilde. Swallow the rest so it is not read as keystrokes. */
            while (b >= '0' && b <= '9') {
                if (!read_byte(fd, &b))
                    break;
            }
            return TP_KEY_NONE;
        }
    }

    switch (c) {
    case 'k': case 'w': case '-':  return TP_KEY_UP;
    case 'j': case 's': case '+':  return TP_KEY_DOWN;
    case '\n': case '\r': case ' ': return TP_KEY_SELECT;
    case 'p':                      return TP_KEY_PLAYPAUSE;
    case 'n':                      return TP_KEY_NEXT;
    case 'b':                      return TP_KEY_PREV;
    case 'x':                      return TP_KEY_STOP;
    case 'q':                      return TP_KEY_QUIT;
    default:                       return TP_KEY_NONE;
    }
}
