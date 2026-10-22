/* cfg.c - INI-like configuration parser for the boot launcher.
 *
 * File format (ASCII text, one entry per application):
 *
 *   # or ; introduce a comment line
 *   [name]                 start a new application entry
 *   cmd = <command line>   what to launch (exactly one per entry)
 *   deps = a, b, c         applications that must start first (optional)
 *   wait = 1000            milliseconds to pause after starting (optional)
 *   cwd = <directory>      initial working directory (optional)
 *   output = <file>        child stdout+stderr appended to file (optional)
 *   type = system|application   startup class (optional, default system)
 *   load = start|stop|auto      boot behaviour: always start / never /
 *                              only when last runtime state was
 *                              running (optional, default start)
 *
 * Portable C89 only: no strdup, no snprintf, declarations at block top.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "log.h"

#define LINE_MAX_LEN 1024

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Duplicate a path value, stripping one layer of surrounding double
 * quotes (the value may contain blanks). NULL for an empty result. */
static char *dup_path(const char *s)
{
    char  *p = dup_str(s);
    size_t n;
    if (!p) return NULL;
    n = strlen(p);
    if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
        p[n - 1] = '\0';
        memmove(p, p + 1, n - 1);
    }
    if (!*p) { free(p); return NULL; }
    return p;
}

/* Trim leading/trailing blanks in place; returns pointer to content. */
static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' '  || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = '\0';
    return s;
}

int cfg_index(const app_t *apps, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(apps[i].name, name) == 0) return i;
    return -1;
}

int app_kind(const app_t *a)
{
    return (a->type && strcmp(a->type, "application") == 0)
         ? KIND_APP : KIND_SYSTEM;
}

int app_load(const app_t *a)
{
    if (!a->load || strcmp(a->load, "start") == 0) return LOAD_START;
    if (strcmp(a->load, "stop") == 0)              return LOAD_STOP;
    return LOAD_AUTO;
}

void cfg_free(app_t *apps, int n)
{
    int i, k;
    if (!apps) return;
    for (i = 0; i < n; i++) {
        for (k = 0; k < apps[i].ndeps; k++) free(apps[i].deps[k]);
        free(apps[i].deps);
        free(apps[i].cmd);
        free(apps[i].cwd);
        free(apps[i].output);
        free(apps[i].type);
        free(apps[i].load);
        free(apps[i].name);
    }
    free(apps);
}

/* Append one dependency token to the current entry. Returns 0 on ok. */
static int add_dep(app_t *cur, const char *tok)
{
    char **nd;
    nd = (char **)realloc(cur->deps, (size_t)(cur->ndeps + 1) * sizeof(char *));
    if (!nd) return -1;
    cur->deps = nd;
    cur->deps[cur->ndeps] = dup_str(tok);
    if (!cur->deps[cur->ndeps]) return -1;
    cur->ndeps++;
    return 0;
}

/* Parse the value of a "deps" key: comma/semicolon separated names. */
static int parse_deps(app_t *cur, char *val)
{
    char *p = val;
    while (p) {
        char *sep = strpbrk(p, ",;");
        char *tok;
        if (sep) { *sep = '\0'; tok = trim(p); p = sep + 1; }
        else     { tok = trim(p); p = NULL; }
        if (*tok) {
            if (add_dep(cur, tok) != 0) return -1;
        }
    }
    return 0;
}

int cfg_load(const char *path, app_t **apps_out, int *n_out)
{
    FILE  *f;
    char   line[LINE_MAX_LEN];
    app_t *apps = NULL;
    int    n = 0, cap = 0;
    app_t *cur = NULL;
    int    ln = 0;

    f = fopen(path, "r");
    if (!f) {
        log_msg("cfg: cannot open '%s'", path);
        return -1;
    }

    while (fgets(line, (int)sizeof(line), f)) {
        char *s, *eq, *key, *val;
        ln++;
        s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';') continue;

        if (*s == '[') {                       /* ---- new section ---- */
            char *end = strchr(s, ']');
            app_t *na;
            if (!end) {
                log_msg("cfg: %s:%d: malformed section header", path, ln);
                goto fail;
            }
            *end = '\0';
            s = trim(s + 1);
            if (!*s) {
                log_msg("cfg: %s:%d: empty section name", path, ln);
                goto fail;
            }
            if (cfg_index(apps, n, s) >= 0) {
                log_msg("cfg: %s:%d: duplicate app '%s'", path, ln, s);
                goto fail;
            }
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                na = (app_t *)realloc(apps, (size_t)cap * sizeof(app_t));
                if (!na) goto fail;
                apps = na;
            }
            cur = &apps[n++];
            memset(cur, 0, sizeof(*cur));
            cur->name = dup_str(s);
            if (!cur->name) goto fail;
            continue;
        }

        /* ---- key = value inside a section ---- */
        if (!cur) {
            log_msg("cfg: %s:%d: key outside of any [section]", path, ln);
            goto fail;
        }
        eq = strchr(s, '=');
        if (!eq) {
            log_msg("cfg: %s:%d: missing '=' in '%s'", path, ln, s);
            goto fail;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);

        if (strcmp(key, "cmd") == 0) {
            if (cur->cmd) {
                log_msg("cfg: %s:%d: duplicate cmd for app '%s'",
                        path, ln, cur->name);
                goto fail;
            }
            if (!*val) {
                log_msg("cfg: %s:%d: empty cmd for app '%s'",
                        path, ln, cur->name);
                goto fail;
            }
            cur->cmd = dup_str(val);
            if (!cur->cmd) goto fail;
        } else if (strcmp(key, "deps") == 0) {
            if (parse_deps(cur, val) != 0) goto fail;
        } else if (strcmp(key, "wait") == 0) {
            cur->wait_ms = atoi(val);
            if (cur->wait_ms < 0) cur->wait_ms = 0;
        } else if (strcmp(key, "cwd") == 0 || strcmp(key, "output") == 0) {
            char **slot = (strcmp(key, "cwd") == 0) ? &cur->cwd
                                                    : &cur->output;
            if (*slot) {
                log_msg("cfg: %s:%d: duplicate %s for app '%s'",
                        path, ln, key, cur->name);
                goto fail;
            }
            if (!*val) {
                log_msg("cfg: %s:%d: empty %s for app '%s'",
                        path, ln, key, cur->name);
                goto fail;
            }
            *slot = dup_path(val);
            if (!*slot) {
                log_msg("cfg: %s:%d: blank %s for app '%s'",
                        path, ln, key, cur->name);
                goto fail;
            }
        } else if (strcmp(key, "type") == 0) {
            if (cur->type) {
                log_msg("cfg: %s:%d: duplicate type for app '%s'",
                        path, ln, cur->name);
                goto fail;
            }
            if (strcmp(val, "system") != 0 &&
                strcmp(val, "application") != 0) {
                log_msg("cfg: %s:%d: type must be system or "
                        "application (got '%s')", path, ln, val);
                goto fail;
            }
            cur->type = dup_str(val);
            if (!cur->type) goto fail;
        } else if (strcmp(key, "load") == 0) {
            if (cur->load) {
                log_msg("cfg: %s:%d: duplicate load for app '%s'",
                        path, ln, cur->name);
                goto fail;
            }
            if (strcmp(val, "start") != 0 &&
                strcmp(val, "stop") != 0 &&
                strcmp(val, "auto") != 0) {
                log_msg("cfg: %s:%d: load must be start, stop or "
                        "auto (got '%s')", path, ln, val);
                goto fail;
            }
            cur->load = dup_str(val);
            if (!cur->load) goto fail;
        } else {
            log_msg("cfg: %s:%d: unknown key '%s'", path, ln, key);
            goto fail;
        }
    }
    fclose(f);
    f = NULL;

    /* every entry must carry a command line */
    {
        int i;
        for (i = 0; i < n; i++) {
            if (!apps[i].cmd) {
                log_msg("cfg: %s: app '%s' has no cmd= line",
                        path, apps[i].name);
                goto fail;
            }
        }
    }

    *apps_out = apps;
    *n_out = n;
    return 0;

fail:
    if (f) fclose(f);
    cfg_free(apps, n);
    return -1;
}

/* ------------------------------------------------------------------ */
/* in-memory model editing (shared by CLI and GUI)                      */
/* ------------------------------------------------------------------ */

int cfg_add(app_t **apps, int *n, const char *name)
{
    app_t *na;

    if (!*name || cfg_index(*apps, *n, name) >= 0) return -1;
    na = (app_t *)realloc(*apps, (size_t)(*n + 1) * sizeof(app_t));
    if (!na) return -1;
    *apps = na;
    memset(&(*apps)[*n], 0, sizeof(app_t));
    (*apps)[*n].name = dup_str(name);
    if (!(*apps)[*n].name) return -1;
    (*apps)[*n].cmd = dup_str("");
    if (!(*apps)[*n].cmd) {
        free((*apps)[*n].name);
        (*apps)[*n].name = NULL;
        return -1;
    }
    return (*n)++;
}

int cfg_remove(app_t **apps, int *n, const char *name)
{
    int    i = cfg_index(*apps, *n, name);
    int    j, k, m;
    app_t *na;

    if (i < 0) return -1;
    for (k = 0; k < (*apps)[i].ndeps; k++) free((*apps)[i].deps[k]);
    free((*apps)[i].deps);
    free((*apps)[i].cmd);
    free((*apps)[i].cwd);
    free((*apps)[i].output);
    free((*apps)[i].type);
    free((*apps)[i].load);
    free((*apps)[i].name);
    memmove(&(*apps)[i], &(*apps)[i + 1],
            (size_t)(*n - i - 1) * sizeof(app_t));
    (*n)--;

    /* drop dangling references to the removed app */
    for (j = 0; j < *n; j++) {
        m = 0;
        for (k = 0; k < (*apps)[j].ndeps; k++) {
            if (strcmp((*apps)[j].deps[k], name) == 0)
                free((*apps)[j].deps[k]);
            else
                (*apps)[j].deps[m++] = (*apps)[j].deps[k];
        }
        (*apps)[j].ndeps = m;
    }

    if (*n == 0) { free(*apps); *apps = NULL; }
    else {
        na = (app_t *)realloc(*apps, (size_t)*n * sizeof(app_t));
        if (na) *apps = na;   /* shrink; keeping the larger block is ok */
    }
    return 0;
}

int cfg_rename(app_t *apps, int n, const char *oldname, const char *newname)
{
    int i = cfg_index(apps, n, oldname);
    int j, k;

    if (i < 0 || !*newname || cfg_index(apps, n, newname) >= 0) return -1;
    {
        char *nn = dup_str(newname);
        if (!nn) return -1;
        free(apps[i].name);
        apps[i].name = nn;
    }
    for (j = 0; j < n; j++) {
        if (j == i) continue;
        for (k = 0; k < apps[j].ndeps; k++) {
            if (strcmp(apps[j].deps[k], oldname) == 0) {
                free(apps[j].deps[k]);
                apps[j].deps[k] = dup_str(newname);
            }
        }
    }
    return 0;
}

int cfg_set_field(app_t *a, const char *key, const char *val)
{
    if (strcmp(key, "cmd") == 0) {
        char *s;
        if (!*val) return -1;
        s = dup_str(val);
        if (!s) return -1;
        free(a->cmd);
        a->cmd = s;
        return 0;
    }
    if (strcmp(key, "wait") == 0) {
        int ms = atoi(val);
        a->wait_ms = ms < 0 ? 0 : ms;
        return 0;
    }
    if (strcmp(key, "cwd") == 0 || strcmp(key, "output") == 0) {
        char **slot = (strcmp(key, "cwd") == 0) ? &a->cwd : &a->output;
        char  *s = NULL;
        if (*val) s = dup_path(val); /* blank after unquote = clear */
        free(*slot);
        *slot = s;
        return 0;
    }
    if (strcmp(key, "type") == 0) {
        if (!*val) { free(a->type); a->type = NULL; return 0; }
        if (strcmp(val, "system") != 0 &&
            strcmp(val, "application") != 0) return -1;
        {
            char *s = dup_str(val);
            if (!s) return -1;
            free(a->type);
            a->type = s;
        }
        return 0;
    }
    if (strcmp(key, "load") == 0) {
        if (!*val) { free(a->load); a->load = NULL; return 0; }
        if (strcmp(val, "start") != 0 && strcmp(val, "stop") != 0 &&
            strcmp(val, "auto") != 0) return -1;
        {
            char *s = dup_str(val);
            if (!s) return -1;
            free(a->load);
            a->load = s;
        }
        return 0;
    }
    return -1;
}

int cfg_add_dep(app_t *apps, int n, int idx, const char *dep)
{
    int k;
    if (idx < 0 || idx >= n) return -1;
    if (cfg_index(apps, n, dep) < 0) return -1;   /* must exist */
    for (k = 0; k < apps[idx].ndeps; k++)
        if (strcmp(apps[idx].deps[k], dep) == 0) return 0;  /* already */
    if (add_dep(&apps[idx], dep) != 0) return -1;
    return 0;
}

int cfg_del_dep(app_t *apps, int n, int idx, const char *dep)
{
    int k, m = 0;
    if (idx < 0 || idx >= n) return -1;
    for (k = 0; k < apps[idx].ndeps; k++) {
        if (strcmp(apps[idx].deps[k], dep) == 0) free(apps[idx].deps[k]);
        else apps[idx].deps[m++] = apps[idx].deps[k];
    }
    if (m == apps[idx].ndeps) return -1;   /* nothing removed */
    apps[idx].ndeps = m;
    return 0;
}

int cfg_save(const char *path, const app_t *apps, int n)
{
    FILE *f;
    int   i, k;

    f = fopen(path, "wb");
    if (!f) {
        log_msg("cfg: cannot write '%s'", path);
        return -1;
    }
    fputs("# startup.conf - application startup configuration\r\n"
          "# generated by startup; edit with the CLI or the GUI\r\n"
          "# [name] entry; cmd = command line; "
          "deps = comma separated prerequisites; wait = ms after start\r\n"
          "# optional per app: cwd = working directory; "
          "output = file for stdout+stderr; "
          "type = system or application\r\n"
          "# load = start, stop or auto: always written (default start)\r\n\r\n",
          f);
    for (i = 0; i < n; i++) {
        fprintf(f, "[%s]\r\n", apps[i].name);
        fprintf(f, "cmd = %s\r\n", apps[i].cmd ? apps[i].cmd : "");
        if (apps[i].type)
            fprintf(f, "type = %s\r\n", apps[i].type);
        /* boot policy is always explicit: the effective default value
         * is written out so every entry states its own boot behaviour */
        fprintf(f, "load = %s\r\n",
                app_load(&apps[i]) == LOAD_STOP ? "stop" :
                app_load(&apps[i]) == LOAD_AUTO ? "auto" : "start");
        if (apps[i].cwd)
            fprintf(f, "cwd = %s\r\n", apps[i].cwd);
        if (apps[i].output)
            fprintf(f, "output = %s\r\n", apps[i].output);
        if (apps[i].ndeps > 0) {
            fputs("deps =", f);
            for (k = 0; k < apps[i].ndeps; k++)
                fprintf(f, "%s %s", k ? "," : "", apps[i].deps[k]);
            fputs("\r\n", f);
        }
        if (apps[i].wait_ms > 0)
            fprintf(f, "wait = %d\r\n", apps[i].wait_ms);
        fputs("\r\n", f);
    }
    fclose(f);
    return 0;
}
