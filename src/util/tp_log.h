#ifndef TP_LOG_H
#define TP_LOG_H

#include <stdarg.h>

enum tp_log_level {
    TP_LOG_ERROR = 0,
    TP_LOG_WARN,
    TP_LOG_INFO,
    TP_LOG_DEBUG
};

void tp_log_set_level(enum tp_log_level level);
enum tp_log_level tp_log_get_level(void);
void tp_log(enum tp_log_level level, const char *fmt, ...);
void tp_logv(enum tp_log_level level, const char *fmt, va_list ap);

#define tp_error(...) tp_log(TP_LOG_ERROR, __VA_ARGS__)
#define tp_warn(...)  tp_log(TP_LOG_WARN, __VA_ARGS__)
#define tp_info(...)  tp_log(TP_LOG_INFO, __VA_ARGS__)
#define tp_debug(...) tp_log(TP_LOG_DEBUG, __VA_ARGS__)

#endif
