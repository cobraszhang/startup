/* state.h - persistent record of each entry's last runtime state.
 *
 * The supervisor appends one line per state transition (a program
 * started, or it exited / was stopped) to a state log next to the
 * main log file. "load = auto" entries consult this log at boot:
 * start only when the last recorded state was running. A crash adds
 * no event, so the last transition line still reflects reality.
 *
 * Line format (append-only, last line per name wins):
 *   2026-10-22 10:00:00 state=running name=<app>
 */
#ifndef STATE_H
#define STATE_H

#include <stddef.h>

#define ST_RUNNING 1
#define ST_STOPPED 0
#define ST_UNKNOWN (-1)

/* Resolve the state log path once at startup:
 *   dir(log_file) / startup-state.log   when the runtime log was sent
 *                                       to a file ($BOOTLAUNCHER_LOG)
 *   else <exe_dir>/../logs/startup-state.log   (release layout)
 * The directory must already exist (each side's entry creates it);
 * when no path can be derived, recording is disabled. */
void           st_init(const char *log_file, const char *exe_dir);

/* Active state log path, NULL when recording is disabled. */
const char    *st_file(void);

/* Append one transition record. 0 = written, -1 = failed/disabled. */
int            st_record(const char *name, int running);

/* Last recorded state for an entry: ST_RUNNING / ST_STOPPED /
 * ST_UNKNOWN (never recorded). */
int            st_last(const char *name);

#endif /* STATE_H */
