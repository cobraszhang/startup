/* log.h - minimal timestamped logging to stderr (portable C89).
 * A GUI front-end may install a sink to receive formatted lines instead. */
#ifndef LOG_H
#define LOG_H

typedef void (*log_sink_fn)(const char *line);

/* Install (or clear with NULL) an alternative output sink. */
void log_set_sink(log_sink_fn f);

void log_msg(const char *fmt, ...);

#endif /* LOG_H */
