#include "tp_log.h"

#include <stdio.h>
#include <string.h>

static enum tp_log_level g_level = TP_LOG_INFO;

void tp_log_set_level(enum tp_log_level level)
{
    g_level = level;
}

enum tp_log_level tp_log_get_level(void)
{
    return g_level;
}

void tp_logv(enum tp_log_level level, const char *fmt, va_list ap)
{
    const char *tag;
    if (level > g_level)
        return;
    switch (level) {
    case TP_LOG_ERROR: tag = "error"; break;
    case TP_LOG_WARN:  tag = "warn"; break;
    case TP_LOG_INFO:  tag = "info"; break;
    default:           tag = "debug"; break;
    }
    if (level <= TP_LOG_WARN || g_level >= TP_LOG_DEBUG) {
        fprintf(stderr, "tinypod: %s: ", tag);
        vfprintf(stderr, fmt, ap);
        if (fmt[0] && fmt[strlen(fmt) - 1] != '\n')
            fputc('\n', stderr);
    } else if (level == TP_LOG_INFO) {
        vfprintf(stdout, fmt, ap);
        if (fmt[0] && fmt[strlen(fmt) - 1] != '\n')
            fputc('\n', stdout);
    }
}

void tp_log(enum tp_log_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    tp_logv(level, fmt, ap);
    va_end(ap);
}
