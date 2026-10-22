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
#include "ipc.h"

static const char *g_cmds[] = {
    "list", "order", "check", "run",
    "add", "rm", "rename", "set", "dep", "help", "version", "app"
};

/* control-channel endpoint override for the 'app' subcommand */
static char g_endpoint[512];

void cli_set_endpoint(const char *endpoint)
{
    if (!endpoint || !*endpoint) return;
    strncpy(g_endpoint, endpoint, sizeof(g_endpoint) - 1);
    g_endpoint[sizeof(g_endpoint) - 1] = '\0';
}

/* 'app' subcommands: forwarded to the running supervisor(s).
 * Both the system-side and the user-side supervisor answer; LIST and
 * STATUS merge their replies, name-targeted verbs are retried against
 * the other side when the first answer is an error, and QUIT stops both. */
static int ipc_one(const char *ep, const char *req, char *reply, size_t rsz)
{
    ipc_conn *c;
    int       got;

    c = ipc_client_open(ep);
    if (!c) return 3;                       /* endpoint not present */
    got = ipc_client_call(c, req, reply, rsz);
    ipc_client_close(c);
    if (got < 0) return 1;                  /* transport error */
    return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
}

static void ep_for(char *out, size_t sz, int kind)
{
    size_t n;

    if (!g_endpoint[0]) {
        ipc_default_endpoint(out, sz, kind);
        return;
    }
    n = strlen(g_endpoint);
    if (n > sz - 1) n = sz - 1;
    memcpy(out, g_endpoint, n);
    out[n] = '\0';
}

static int build_req(char *req, size_t rsz, const char *verb,
                     const char *name)
{
    size_t n = strlen(verb);
    if (n + 1 >= rsz) return -1;
    memcpy(req, verb, n);
    req[n] = '\0';
    if (*name) {
        req[n++] = ' ';
        strncpy(req + n, name, rsz - n - 1);
        req[rsz - 1] = '\0';
    }
    return 0;
}

/* print reply, optionally dropping its first "OK <header>" line so a
 * second supervisor's output continues the same listing */
static void show_reply(const char *reply, int drop_header)
{
    const char *p = reply;
    if (drop_header) {
        const char *nl = strchr(reply, '\n');
        if (!nl) return;
        p = nl + 1;
    }
    fputs(p, stdout);
}

static int cmd_app(int argc, char **argv)
{
    static const struct {
        const char *sc;     /* subcommand as typed by the user   */
        const char *verb;   /* protocol verb sent to the service */
        int         named;  /* requires an application name      */
    } tbl[] = {
        { "ping",     "PING",    0 },
        { "list",     "LIST",    0 },
        { "status",   "STATUS",  0 },
        { "start",    "START",   1 },
        { "stop",     "STOP",    1 },
        { "restart",  "RESTART", 1 },
        { "shutdown", "QUIT",    0 }
    };
    char        ep[512], req[IPC_LINE_MAX], reply[8192], ep2[512];
    const char *name;
    size_t      i;
    int         rc;

    if (argc < 2) {
        fprintf(stderr, "usage: app ping|list [name]|start|stop|restart"
                        " <name>|status [name]|shutdown\n");
        return 2;
    }
    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(argv[1], tbl[i].sc) == 0) break;
    if (i >= sizeof(tbl) / sizeof(tbl[0])) {
        fprintf(stderr, "app: unknown control subcommand '%s'\n", argv[1]);
        return 2;
    }
    name = (argc >= 3) ? argv[2] : "";
    if (tbl[i].named && !*name) {
        fprintf(stderr, "usage: app %s <name>\n", argv[1]);
        return 2;
    }
    if (build_req(req, sizeof(req), tbl[i].verb, name) != 0) {
        fprintf(stderr, "app: request too long\n");
        return 2;
    }

    /* merged / both-sides verbs: list, status (no name), shutdown;
     * a named status behaves like the other name-targeted verbs */
    if (!tbl[i].named && !*name && strcmp(tbl[i].sc, "ping") != 0) {
        int any = 0, r1, r2;
        ep_for(ep, sizeof(ep), KIND_SYSTEM);
        r1 = ipc_one(ep, req, reply, sizeof(reply));
        if (r1 != 3) {
            any = 1;
            show_reply(reply, 0);
            if (r1 != 0) rc = 1; else rc = 0;
        } else {
            rc = 3;
        }
        ep_for(ep2, sizeof(ep2), KIND_APP);
        if (strcmp(ep2, ep) != 0) {
            r2 = ipc_one(ep2, req, reply, sizeof(reply));
            if (r2 != 3) {
                any = 1;
                show_reply(reply, (r1 != 3));
                if (r2 != 0 && rc == 0) rc = 1;
            }
        }
        if (!any)
            log_msg("app: cannot reach the control service at %s "
                    "(is the service process running?)", ep);
        return any ? rc : 3;
    }

    /* ping, or name-targeted verbs: try the system side first, then
     * the user side (an app is managed by the supervisor of its class) */
    ep_for(ep, sizeof(ep), KIND_SYSTEM);
    rc = ipc_one(ep, req, reply, sizeof(reply));
    if (rc != 0) {
        ep_for(ep2, sizeof(ep2), KIND_APP);
        if (strcmp(ep2, ep) != 0) {
            int r2 = ipc_one(ep2, req, reply, sizeof(reply));
            if (r2 == 0 || (rc == 3 && r2 != 3)) rc = r2;
        }
    }
    if (rc == 3) {
        log_msg("app: cannot reach the control service at %s "
                "(is the service process running?)", ep);
        return 3;
    }
    if (rc == 1) {
        fputs(reply, stdout);
        return 1;
    }
    fputs(reply, stdout);
    return 0;
}

int cli_is_known(const char *cmd)
{
    size_t i;
    for (i = 0; i < sizeof(g_cmds) / sizeof(g_cmds[0]); i++)
        if (strcmp(g_cmds[i], cmd) == 0) return 1;
    return 0;
}

void cli_usage(const char *prog)
{
    fprintf(stderr, "startup %s\n", APPBOOT_VERSION);
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
        "  set <name> wait <ms>     set post-start wait in ms\n");
    fprintf(stderr,
        "  set <name> cwd <dir>     set working directory ('' clears)\n"
        "  set <name> output <f>    capture stdout+stderr ('' clears)\n"
        "  set <name> type <t>      system | application (startup class)\n"
        "  dep add <a> <b>          make application a depend on b\n"
        "  dep del <a> <b>          remove that dependency\n"
        "  help                     this text\n"
        "  version                  print the release version\n");
    fprintf(stderr,
        "  app ping|list|status [name] |start|stop|restart <name>|shutdown\n"
        "                        control running apps via the service "
        "process (IPC)\n");
    fprintf(stderr, "example: %s list   (config selected with -c FILE)\n",
            prog);
}

static int print_order(const app_t *apps, int n)
{
    int *order = NULL;
    int  cnt = 0;
    int  rc = topo_sort(apps, n, TOPO_ALL, &order, &cnt);
    int  i;

    if (rc != 0) return rc;
    for (i = 0; i < cnt; i++)
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
        printf("startup %s\n", APPBOOT_VERSION);
        return 0;
    }
    if (strcmp(cmd, "app") == 0)
        return cmd_app(argc, argv);
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
            printf("  type = %s\n",
                   app_kind(&apps[i]) == KIND_APP ? "application"
                                                 : "system");
            if (apps[i].cwd)
                printf("  cwd  = %s\n", apps[i].cwd);
            if (apps[i].output)
                printf("  out  = %s\n", apps[i].output);
            printf("  deps =");
            for (k = 0; k < apps[i].ndeps; k++)
                printf(" %s", apps[i].deps[k]);
            printf("\n  wait = %d\n", apps[i].wait_ms);
        }
    } else if (strcmp(cmd, "check") == 0) {
        int *o = NULL;
        int  cnt = 0;
        if (topo_sort(apps, n, TOPO_ALL, &o, &cnt) != 0) rc = 1;
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
                "usage: set <name> cmd <line...> | set <name>"
                " wait|cwd|output|type <value>\n");
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
            log_msg("cli: set: key must be cmd/wait/cwd/output/type with "
                    "a valid value (type: system|application)");
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
