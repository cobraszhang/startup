/* cfg.h - application startup configuration parser (portable C89) */
#ifndef CFG_H
#define CFG_H

typedef struct app {
    char  *name;     /* unique application id, from [section] header   */
    char  *cmd;      /* command line to launch                         */
    char **deps;     /* names that must be started before this app     */
    int    ndeps;    /* number of entries in deps                      */
    int    wait_ms;  /* pause after starting this app (before others)  */
} app_t;

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

/* Set "cmd" or "wait" field of one app. */
int  cfg_set_field(app_t *a, const char *key, const char *val);

/* Add/remove a dependency; dep must name an existing app (add). */
int  cfg_add_dep(app_t *apps, int n, int idx, const char *dep);
int  cfg_del_dep(app_t *apps, int n, int idx, const char *dep);

/* Write the model back in canonical form (CRLF, ASCII header). */
int  cfg_save(const char *path, const app_t *apps, int n);

#endif /* CFG_H */
