/* cfg.h - application startup configuration parser (portable C89) */
#ifndef CFG_H
#define CFG_H

typedef struct app {
    char  *name;     /* unique application id, from [section] header   */
    char  *cmd;      /* command line to launch                         */
    char **deps;     /* names that must be started before this app     */
    int    ndeps;    /* number of entries in deps                      */
    int    wait_ms;  /* pause after starting this app (before others)  */
    char  *cwd;      /* initial working directory, NULL = inherit      */
    char  *output;   /* file for child stdout+stderr, NULL = none      */
    char  *type;     /* "system"/"application", NULL = "system"        */
    char  *load;     /* "start"/"stop"/"auto", NULL = "start"         */
} app_t;

/* startup class of one entry: KIND_SYSTEM entries are started by the
 * system-side launcher (Win32 service / systemd unit), KIND_APP entries
 * by the user-side launcher (Win32 Run entry / user autostart).
 * User applications must never start before system services. */
#define KIND_SYSTEM 0
#define KIND_APP    1
int  app_kind(const app_t *a);  /* from a->type; default system */

/* boot-time load behaviour (independent of the startup class):
 * LOAD_START always start at boot; LOAD_STOP never start at boot;
 * LOAD_AUTO start at boot only when the persistent state log shows
 * the entry was last running (see src/state.h). */
#define LOAD_START 0
#define LOAD_STOP  1
#define LOAD_AUTO  2
int  app_load(const app_t *a);  /* from a->load; default start */

/* Load configuration from file. Returns 0 on success and stores an
 * array of n apps in *apps_out; returns non-zero on any parse error. */
int  cfg_load(const char *path, app_t **apps_out, int *n_out);

/* Release everything allocated by cfg_load. */
void cfg_free(app_t *apps, int n);

/* Index of the app with the given name, or -1 when not found. */
int  cfg_index(const app_t *apps, int n, const char *name);

/* ---- in-memory model editing (used by the CLI and the GUI) ---- */

/* Append entry with empty cmd. Returns its index, or -1 (exists / OOM). */
int  cfg_add(app_t **apps, int *n, const char *name);

/* Remove entry and drop references to it from other deps lists. */
int  cfg_remove(app_t **apps, int *n, const char *name);

/* Rename entry; other apps' dependency references follow. */
int  cfg_rename(app_t *apps, int n, const char *oldname, const char *newname);

/* Set "cmd", "wait", "cwd", "output", "type" or "load" field of one
 * app (empty value clears cwd/output/type/load; type must be "system"
 * or "application"; load must be "start", "stop" or "auto"). */
int  cfg_set_field(app_t *a, const char *key, const char *val);

/* Add/remove a dependency; dep must name an existing app (add). */
int  cfg_add_dep(app_t *apps, int n, int idx, const char *dep);
int  cfg_del_dep(app_t *apps, int n, int idx, const char *dep);

/* Write the model back in canonical form (CRLF, ASCII header). */
int  cfg_save(const char *path, const app_t *apps, int n);

#endif /* CFG_H */
