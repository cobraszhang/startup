/* log.c - timestamped logging to stderr, or to an installed sink.
 * Portable C89. */
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "log.h"

#define LOG_BUF_MAX 8192

static log_sink_fn g_sink = NULL;

void log_set_sink(log_sink_fn f)
{
    g_sink = f;
}

void log_msg(const char *fmt, ...)
{
    char        buf[LOG_BUF_MAX];
    va_list     ap;
    time_t      t;
    struct tm  *tmv;
    char        stamp[32];
    int         off = 0;

    t = time(NULL);
    tmv = localtime(&t);
    if (tmv && strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tmv) > 0)
        off = sprintf(buf, "[%s] ", stamp);

    va_start(ap, fmt);
    vsprintf(buf + off, fmt, ap);
    va_end(ap);

    if (g_sink) {
        g_sink(buf);
    } else {
        fprintf(stderr, "%s\n", buf);
        fflush(stderr);
    }
}
