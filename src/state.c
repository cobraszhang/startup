/* state.c - see state.h. Portable C89 + plain stdio only, shared by
 * the Win32 and POSIX supervisors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "state.h"

static char g_path[4096];

void st_init(const char *log_file, const char *exe_dir)
{
    const char *base = NULL;
    int         from_log = 0;

    g_path[0] = '\0';
    if (log_file && *log_file) {
        base = log_file;
        from_log = 1;
    } else if (exe_dir && *exe_dir) {
        base = exe_dir;
    }
    if (!base) return;

    if (from_log) {
        const char *cut = NULL, *p;
        for (p = base; *p; p++)
            if (*p == '/' || *p == '\\') cut = p;
        if (!cut) return;   /* no directory part: nothing sane to do */
        if ((size_t)(cut - base) >= sizeof(g_path) - 24) return;
        memcpy(g_path, base, (size_t)(cut - base));
        g_path[cut - base] = '\0';
        strcat(g_path, "/startup-state.log");
    } else {
        if (strlen(base) + strlen("/../logs/startup-state.log")
                >= sizeof(g_path)) return;
        strcpy(g_path, base);
        strcat(g_path, "/../logs/startup-state.log");
    }
}

const char *st_file(void)
{
    return g_path[0] ? g_path : NULL;
}

int st_record(const char *name, int running)
{
    FILE *f;
    time_t t;
    struct tm *tmv;
    char stamp[32];

    if (!g_path[0] || !name) return -1;
    f = fopen(g_path, "a");
    if (!f) return -1;
    stamp[0] = '\0';
    t = time(NULL);
    tmv = localtime(&t);
    if (tmv) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tmv);
    fprintf(f, "%s state=%s name=%s\n", stamp,
            running ? "running" : "stopped", name);
    fclose(f);
    return 0;
}

/* one line of the state log -> (matches?, state) */
static int line_match(const char *line, const char *name, int *state)
{
    const char *sv, *nm;
    const char *end;

    *state = -1;
    sv = strstr(line, " state=");
    nm = strstr(line, " name=");
    if (!sv || !nm) return 0;
    sv += 7;                              /* the value after " state=" */
    if (strncmp(nm + 6, name, strlen(name)) != 0) return 0;
    end = nm + 6 + (int)strlen(name);
    while (*end == '\r' || *end == '\n') end++;
    if (*end != '\0') return 0;          /* prefix match only: no */
    if (strncmp(sv, "running", 7) == 0) { *state = ST_RUNNING; return 1; }
    if (strncmp(sv, "stopped", 7) == 0) { *state = ST_STOPPED; return 1; }
    return 0;
}

int st_last(const char *name)
{
    FILE *f;
    char  line[512];
    int   last = ST_UNKNOWN;

    if (!g_path[0] || !name) return ST_UNKNOWN;
    f = fopen(g_path, "r");
    if (!f) return ST_UNKNOWN;
    while (fgets(line, sizeof(line), f)) {
        int st;
        if (line_match(line, name, &st)) last = st;
    }
    fclose(f);
    return last;
}
