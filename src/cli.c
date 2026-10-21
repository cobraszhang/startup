/* cli.c - shared command-line management interface (portable C89).
 *
 * Subcommands inspect or modify the configuration file; modifications
 * are saved back through cfg_save() so CLI and GUI produce identical
 * canonical files. Errors go through log_msg() (stderr by default).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "version.h"

static const char *g_cmds[] = {
    "list", "order", "check", "run",
    "add", "rm", "rename", "set", "dep", "help", "version"
};

int cli_is_known(const char *cmd)
{
    size_t i;
    for (i = 0; i < sizeof(g_cmds) / sizeof(g_cmds[0]); i++)
        if (strcmp(g_cmds[i], cmd) == 0) return 1;
    return 0;
}

void cli_usage(const char *prog)
{
    fprintf(stderr, "AppBootLauncher %s\n", APPBOOT_VERSION);
    fprintf(stderr,
        "management subcommands (operate on the configuration file):\n"
        "  list                     list applications\n"
        "  order                    print dependency-resolved start order\n"
        "  check                    validate configuration only\n"
        "  run                      validate, then start all in order\n"
        "  add <name> <cmdline>     add entry with its command line\n");
    fprintf(stderr,
        "  rm <name>                remove entry and references to it\n"
        "  rename <old> <new>       rename entry (references follow)\n"
        "  set <name> cmd <line>    set command line (args may be unquoted)\n"
        "  set <name> wait <ms>     set post-start wait in ms\n"
        "  dep add <a> <b>          make application a depend on b\n"
        "  dep del <a> <b>          remove that dependency\n"
        "  help                     this text\n"
        "  version                  print the release version\n");
    fprintf(stderr, "example: %s list   (config selected with -c FILE)\n",
            prog);
}

static int print_order(const app_t *apps, int n)
{
    int *order = NULL;
    int  rc = topo_sort(apps, n, &order);
    int  i;

    if (rc != 0) return rc;
    for (i = 0; i < n; i++)
        printf("%d. %s\n", i + 1, apps[order[i]].name);
    free(order);
    return 0;
}

static int save_and_report(const char *path, const app_t *apps, int n)
{
    if (cfg_save(path, apps, n) != 0) return 1;
    printf("configuration updated: %s\n", path);
    return 0;
}

/* locate apps[idx] by name; prints an error and returns -1 if missing */
static int find_or_error(const app_t *apps, int n, const char *name)
{
    int idx = cfg_index(apps, n, name);
    if (idx < 0) log_msg("cli: no such application '%s'", name);
    return idx;
}

/* Join argv[from..argc-1] with single spaces (values may be typed
 * unquoted in the shell). Returns NULL on OOM. Caller frees. */
static char *join_args(char **argv, int from, int argc)
{
    size_t total = 0;
    int    i;
    char  *buf, *p;

    for (i = from; i < argc; i++) total += strlen(argv[i]) + 1;
    buf = (char *)malloc(total ? total : 1);
    if (!buf) return NULL;
    p = buf;
    for (i = from; i < argc; i++) {
        if (i > from) *p++ = ' ';
        strcpy(p, argv[i]);
        p += strlen(p);
    }
    *p = '\0';
    return buf;
}

int cli_main(int argc, char **argv, const char *prog,
             const char *confpath, cli_run_fn run_fn)
{
    const char *cmd = argv[0];
    app_t      *apps = NULL;
    int         n = 0, rc = 0, idx = -1;

    if (strcmp(cmd, "help") == 0) {
        cli_usage(prog);
        return 0;
    }
    if (strcmp(cmd, "version") == 0) {
        printf("AppBootLauncher %s\n", APPBOOT_VERSION);
        return 0;
    }
    if (strcmp(cmd, "run") == 0)
        return run_fn(confpath);

    if (cfg_load(confpath, &apps, &n) != 0)
        return 1;

    if (strcmp(cmd, "list") == 0) {
        int i, k;
        if (n == 0) printf("(no applications configured)\n");
        for (i = 0; i < n; i++) {
            printf("%s\n", apps[i].name);
            printf("  cmd  = %s\n", apps[i].cmd);
            printf("  deps =");
            for (k = 0; k < apps[i].ndeps; k++)
                printf(" %s", apps[i].deps[k]);
            printf("\n  wait = %d\n", apps[i].wait_ms);
        }
    } else if (strcmp(cmd, "check") == 0) {
        int *o = NULL;
        if (topo_sort(apps, n, &o) != 0) rc = 1;
        else {
            free(o);
            printf("configuration OK: %d application(s)\n", n);
        }
    } else if (strcmp(cmd, "order") == 0) {
        if (print_order(apps, n) != 0) rc = 1;
    } else if (strcmp(cmd, "add") == 0) {
        char *full;
        if (argc < 3) {
            rc = 2;
            fprintf(stderr, "usage: add <name> <command line...>\n");
        } else if (cfg_add(&apps, &n, argv[1]) < 0) {
            log_msg("cli: cannot add '%s' (exists or out of memory)",
                    argv[1]);
            rc = 1;
        } else if ((full = join_args(argv, 2, argc)) == NULL) {
            log_msg("cli: out of memory");
            rc = 1;
        } else {
            if (cfg_set_field(&apps[n - 1], "cmd", full) != 0) {
                log_msg("cli: empty command line");
                rc = 1;
            } else rc = save_and_report(confpath, apps, n);
            free(full);
        }
    } else if (strcmp(cmd, "rm") == 0) {
        if (argc < 2) { rc = 2; fprintf(stderr, "usage: rm <name>\n"); }
        else if (cfg_remove(&apps, &n, argv[1]) != 0) {
            log_msg("cli: no such application '%s'", argv[1]);
            rc = 1;
        } else rc = save_and_report(confpath, apps, n);
    } else if (strcmp(cmd, "rename") == 0) {
        if (argc < 3) {
            rc = 2;
            fprintf(stderr, "usage: rename <old> <new>\n");
        } else if (cfg_rename(apps, n, argv[1], argv[2]) != 0) {
            log_msg("cli: cannot rename '%s' to '%s' "
                    "(missing or name taken)", argv[1], argv[2]);
            rc = 1;
        } else rc = save_and_report(confpath, apps, n);
    } else if (strcmp(cmd, "set") == 0) {
        if (argc < 4) {
            rc = 2;
            fprintf(stderr,
                "usage: set <name> cmd <line...> | set <name> wait <ms>\n");
        } else if ((idx = find_or_error(apps, n, argv[1])) < 0) {
            rc = 1;
        } else if (strcmp(argv[2], "cmd") == 0) {
            char *full = join_args(argv, 3, argc);
            if (!full) {
                log_msg("cli: out of memory");
                rc = 1;
            } else {
                if (cfg_set_field(&apps[idx], "cmd", full) != 0) {
                    log_msg("cli: empty command line");
                    rc = 1;
                } else rc = save_and_report(confpath, apps, n);
                free(full);
            }
        } else if (cfg_set_field(&apps[idx], argv[2], argv[3]) != 0) {
            log_msg("cli: set: key must be cmd or wait with a value");
            rc = 1;
        } else rc = save_and_report(confpath, apps, n);
    } else if (strcmp(cmd, "dep") == 0) {
        if (argc < 4) {
            rc = 2;
            fprintf(stderr, "usage: dep add|del <a> <b>\n");
        } else if ((idx = find_or_error(apps, n, argv[2])) < 0) {
            rc = 1;
        } else if (strcmp(argv[1], "add") == 0) {
            if (idx >= 0 &&
                strcmp(apps[idx].name, argv[3]) == 0) {
                log_msg("cli: an app cannot depend on itself");
                rc = 1;
            } else if (cfg_add_dep(apps, n, idx, argv[3]) != 0) {
                log_msg("cli: cannot add dependency '%s' (unknown app?)",
                        argv[3]);
                rc = 1;
            } else rc = save_and_report(confpath, apps, n);
        } else if (strcmp(argv[1], "del") == 0) {
            if (cfg_del_dep(apps, n, idx, argv[3]) != 0) {
                log_msg("cli: '%s' does not depend on '%s'",
                        argv[2], argv[3]);
                rc = 1;
            } else rc = save_and_report(confpath, apps, n);
        } else {
            rc = 2;
            fprintf(stderr, "usage: dep add|del <a> <b>\n");
        }
    } else {
        rc = 2;
    }

    cfg_free(apps, n);
    return rc;
}
