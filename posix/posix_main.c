/* posix_main.c - boot-time application launcher for POSIX systems.
 *
 * Intended to be invoked once at boot by init (inittab / rc.local /
 * SysV init.d script) or by a systemd unit. It reads a configuration
 * file, topologically sorts the applications by their "deps" entries
 * and starts them in dependency order with fork + /bin/sh -c.
 *
 * Usage:
 *   startup [-c config-file] [-d]                  boot the applications
 *   startup [-c config-file] serve                 resident supervisor
 *     starts everything, then serves the control IPC (see src/ipc.h) so
 *     user processes can run: startup app start|stop|restart <name>,
 *     startup app status [name], startup app list, app ping, app shutdown
 *   startup [-c config-file] <subcommand> [args]   manage the configuration
 *     -c FILE   configuration to read (default: <exe dir>/../conf/startup.conf,
 *               resolved from /proc/self/exe, falling back to argv[0])
 *     -s NAME   control endpoint for the app subcommand / serve
 *               (default: /tmp/bootlauncher.ctl, env BOOTLAUNCHER_CTL)
 *     -d        detach into the background (double fork + setsid)
 *   Run "startup help" for the management subcommands
 *   (list/order/check/run/add/rm/rename/set/dep/gui).
 *   With no subcommand: when a graphical session (GNOME, or any
 *   X11/Wayland display such as WSLg) is detected the GTK configuration
 *   window opens instead; headless boot sessions (systemd/SysV) keep the
 *   previous behaviour. The GUI is compiled in only when gtk+-3.0
 *   development files are available at build time.
 *
 * Portable C89 + POSIX.1 (fork, exec, setsid, nanosleep, readlink,
 * AF_UNIX sockets, waitpid).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>

#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "cli.h"
#include "version.h"
#include "ipc.h"
#include "posix_app.h"

static void usage(const char *prog)
{
    fprintf(stderr, "AppBootLauncher %s\n", APPBOOT_VERSION);
    fprintf(stderr,
        "usage: %s [-c config-file] [-s endpoint] [-d] [-V] [subcommand ...]\n"
        "  default config: <exe dir>/../conf/startup.conf\n"
        "  -c FILE  configuration file (overrides the default)\n"
        "  -s NAME  control endpoint for 'serve' and 'app' subcommands\n"
        "           (default /tmp/bootlauncher.ctl, env BOOTLAUNCHER_CTL)\n"
        "  -d       detach into the background (boot or serve mode)\n"
        "  -V       print version and exit\n"
        "  (no subcommand) opens the configuration GUI when a graphical\n"
        "  session is detected; otherwise boots the applications\n"
        "  serve     boot + stay resident as the control server\n",
        prog);
    cli_usage(prog);
    fprintf(stderr, "  gui                      force opening the GUI\n");
}

#ifdef HAVE_GUI
/* Non-zero when a graphical session should be usable: a GNOME session
 * is detected first; otherwise any X11/Wayland display (WSLg style). */
static int have_graphical_session(void)
{
    const char *v;

    v = getenv("XDG_CURRENT_DESKTOP");
    if (v && (strstr(v, "GNOME") || strstr(v, "gnome"))) {
        log_msg("GNOME session detected: opening the configuration GUI");
        return 1;
    }
    v = getenv("XDG_SESSION_DESKTOP");
    if (v && strstr(v, "gnome")) {
        log_msg("GNOME session detected: opening the configuration GUI");
        return 1;
    }
    v = getenv("WAYLAND_DISPLAY");
    if (v && *v) {
        log_msg("no GNOME session, but Wayland display '%s': "
                "trying the GUI", v);
        return 1;
    }
    v = getenv("DISPLAY");
    if (v && *v) {
        log_msg("no GNOME session, but X display '%s': trying the GUI", v);
        return 1;
    }
    return 0;
}
#endif

/* Directory holding this executable. Best effort: /proc/self/exe first,
 * argv[0] as fallback (works whenever argv[0] is a path). */
static void exe_dir(const char *argv0, char *out, size_t outsz)
{
    ssize_t  n;
    char    *slash;

    n = readlink("/proc/self/exe", out, outsz - 1);
    if (n > 0) {
        out[n] = '\0';
    } else {
        strncpy(out, (argv0 && *argv0) ? argv0 : ".", outsz - 1);
        out[outsz - 1] = '\0';
    }
    slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    else       strcpy(out, ".");
}

/* can the file be opened for reading? */
static int conf_readable(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* detach from the controlling terminal: classic double fork */
static void daemonize(void)
{
    pid_t p = fork();
    int   fd;

    if (p < 0) {
        log_msg("fork failed");
        _exit(1);
    }
    if (p > 0) _exit(0);          /* original parent exits */

    setsid();                     /* new session, no controlling tty */

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

/* interruptible-ish pause between launches (milliseconds) */
static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000ul);
    nanosleep(&ts, NULL);
}

/* start one application through the shell; parent does not wait */
static int launch_app(const app_t *a)
{
    pid_t p = fork();

    if (p < 0) {
        log_msg("launch: fork failed for '%s'", a->name);
        return -1;
    }
    if (p == 0) {
        execl("/bin/sh", "sh", "-c", a->cmd, (char *)NULL);
        /* only reached when exec failed */
        fprintf(stderr, "exec '%s' failed\n", a->cmd);
        _exit(127);
    }
    log_msg("launch: started '%s' pid=%ld", a->name, (long)p);
    return 0;
}

int run_all(const char *confpath)
{
    app_t *apps = NULL;
    int   *order = NULL;
    int    n = 0, rc, i;

    if (cfg_load(confpath, &apps, &n) != 0) return 1;
    rc = topo_sort(apps, n, &order);
    if (rc != 0) {
        log_msg("startup aborted: dependency ordering failed (code %d)", rc);
        cfg_free(apps, n);
        return 1;
    }

    log_msg("startup: %d application(s), order:", n);
    for (i = 0; i < n; i++)
        log_msg("startup:   %d. %s", i + 1, apps[order[i]].name);

    for (i = 0; i < n; i++) {
        const app_t *a = &apps[order[i]];
        launch_app(a);
        if (a->wait_ms > 0)
            sleep_ms((unsigned)a->wait_ms);
    }

    free(order);
    cfg_free(apps, n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* supervisor: resident control interface for user processes            */
/*                                                                    */
/* `serve' (optionally with -d) loads the model, starts everything in */
/* dependency order, KEEPS the child pids and serves the line protocol */
/* from src/ipc.h on an AF_UNIX socket, so user processes can         */
/* START/STOP/RESTART/STATUS the applications. Plain headless boot    */
/* (no subcommand) keeps the one-shot run_all behaviour.              */
/* ------------------------------------------------------------------ */

typedef struct {
    pid_t pid;
    int   state;   /* 0 not started, 1 running, 2 stopped/exited      */
    int   code;    /* exit status once stopped (wait(2) form)         */
} rt_t;

static app_t  *g_sapps;
static int     g_sn;
static rt_t   *g_srt;
static char    g_ep[512];
static volatile sig_atomic_t g_quit;

static void term_handler(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void rep(char *out, size_t sz, const char *fmt, ...)
{
    char    tmp[512];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    strncat(out, tmp, sz - strlen(out) - 1);
}

static void state_name(int st, char *buf, size_t sz)
{
    if (st == 1)      strncpy(buf, "running",     sz);
    else if (st == 2) strncpy(buf, "stopped",     sz);
    else              strncpy(buf, "not-started", sz);
    buf[sz - 1] = '\0';
}

static int sup_load(const char *confpath)
{
    if (cfg_load(confpath, &g_sapps, &g_sn) != 0)
        return -1;
    g_srt = (rt_t *)calloc((size_t)(g_sn > 0 ? g_sn : 1), sizeof(rt_t));
    if (!g_srt) return -1;
    return 0;
}

static int sup_launch(int i)
{
    pid_t p = fork();

    if (p < 0) {
        log_msg("sup: fork failed for '%s'", g_sapps[i].name);
        return -1;
    }
    if (p == 0) {
        execl("/bin/sh", "sh", "-c", g_sapps[i].cmd, (char *)NULL);
        fprintf(stderr, "exec '%s' failed\n", g_sapps[i].cmd);
        _exit(127);
    }
    g_srt[i].pid   = p;
    g_srt[i].state = 1;
    g_srt[i].code  = -1;
    log_msg("sup: started '%s' pid=%ld", g_sapps[i].name, (long)p);
    return 0;
}

static void sup_boot(void)
{
    int *order = NULL;
    int  i, rc;

    rc = topo_sort(g_sapps, g_sn, &order);
    if (rc != 0) {
        log_msg("startup aborted: dependency ordering failed (code %d)", rc);
        return;
    }
    log_msg("startup: %d application(s), order:", g_sn);
    for (i = 0; i < g_sn; i++)
        log_msg("startup:   %d. %s", i + 1, g_sapps[order[i]].name);

    for (i = 0; i < g_sn && !g_quit; i++) {
        sup_launch(order[i]);
        if (g_sapps[order[i]].wait_ms > 0)
            sleep_ms((unsigned)g_sapps[order[i]].wait_ms);
    }
    free(order);
}

/* refresh and return the state of entry i (reaps natural exits) */
static int rt_state(int i)
{
    if (g_srt[i].state == 1) {
        int   st = 0;
        pid_t r  = waitpid(g_srt[i].pid, &st, WNOHANG);
        if (r == g_srt[i].pid) {
            g_srt[i].state = 2;
            g_srt[i].code  = st;
        } else if (r < 0) {
            /* ECHILD: never waitable (already reaped elsewhere) */
            g_srt[i].state = 2;
        }
    }
    return g_srt[i].state;
}

/* SIGTERM, then SIGKILL after ~2 s; caller marks state */
static void sup_stop(int i)
{
    int k;

    kill(g_srt[i].pid, SIGTERM);
    for (k = 0; k < 20; k++) {
        int   st = 0;
        pid_t r  = waitpid(g_srt[i].pid, &st, WNOHANG);
        if (r == g_srt[i].pid) { g_srt[i].code = st; break; }
        if (r < 0) break;
        sleep_ms(100);
    }
    if (g_srt[i].code == -1) {
        kill(g_srt[i].pid, SIGKILL);
        waitpid(g_srt[i].pid, &g_srt[i].code, 0);
    }
    g_srt[i].state = 2;
}

static int svc_ops(int op, const char *name, char *out, size_t sz)
{
    int idx;

    out[0] = '\0';
    switch (op) {
    case IPC_OP_PING:
        rep(out, sz, "OK PONG AppBootLauncher %s\n", APPBOOT_VERSION);
        break;
    case IPC_OP_LIST:
        rep(out, sz, "OK configured applications:\n");
        for (idx = 0; idx < g_sn; idx++)
            rep(out, sz, "  %-20s cmd=%s\n",
                g_sapps[idx].name, g_sapps[idx].cmd);
        break;
    case IPC_OP_STATUS:
        if (*name) {
            char st[16];
            idx = cfg_index(g_sapps, g_sn, name);
            if (idx < 0) { rep(out, sz, "ERR unknown app '%s'\n", name);
                           break; }
            state_name(rt_state(idx), st, sizeof(st));
            rep(out, sz, "OK status %s:\n  %s %s pid=%ld\n",
                name, g_sapps[idx].name, st, (long)g_srt[idx].pid);
        } else {
            rep(out, sz, "OK status:\n");
            for (idx = 0; idx < g_sn; idx++) {
                char st[16];
                state_name(rt_state(idx), st, sizeof(st));
                rep(out, sz, "  %-20s %s pid=%ld\n",
                    g_sapps[idx].name, st, (long)g_srt[idx].pid);
            }
        }
        break;
    case IPC_OP_START:
        idx = cfg_index(g_sapps, g_sn, name);
        if (idx < 0)                   rep(out, sz, "ERR unknown app '%s'\n", name);
        else if (rt_state(idx) == 1)   rep(out, sz, "ERR '%s' already running (pid=%ld)\n",
                                            name, (long)g_srt[idx].pid);
        else if (sup_launch(idx) != 0) rep(out, sz, "ERR launch failed for '%s'\n", name);
        else                           rep(out, sz, "OK started '%s' pid=%ld\n",
                                            name, (long)g_srt[idx].pid);
        break;
    case IPC_OP_STOP:
        idx = cfg_index(g_sapps, g_sn, name);
        if (idx < 0)                 rep(out, sz, "ERR unknown app '%s'\n", name);
        else if (rt_state(idx) != 1) rep(out, sz, "ERR '%s' not running\n", name);
        else {
            sup_stop(idx);
            log_msg("sup: stopped '%s' pid=%ld", g_sapps[idx].name,
                    (long)g_srt[idx].pid);
            rep(out, sz, "OK stopped '%s'\n", name);
        }
        break;
    case IPC_OP_RESTART:
        idx = cfg_index(g_sapps, g_sn, name);
        if (idx < 0)              rep(out, sz, "ERR unknown app '%s'\n", name);
        else {
            if (rt_state(idx) == 1) sup_stop(idx);
            if (sup_launch(idx) != 0)
                rep(out, sz, "ERR restart failed for '%s'\n", name);
            else
                rep(out, sz, "OK restarted '%s' pid=%ld\n",
                    name, (long)g_srt[idx].pid);
        }
        break;
    case IPC_OP_QUIT:
        g_quit = 1;
        rep(out, sz, "OK bye\n");
        break;
    default:
        rep(out, sz, "ERR internal\n");
        break;
    }
    return op;
}

static void serve_loop(void)
{
    void *srv;
    char  line[IPC_LINE_MAX], out[8192];

    if (!g_ep[0]) ipc_default_endpoint(g_ep, sizeof(g_ep));
    signal(SIGTERM, term_handler);
    signal(SIGINT,  term_handler);

    srv = ipc_server_open(g_ep);
    if (!srv) {
        log_msg("sup: cannot create control endpoint '%s'", g_ep);
        return;
    }
    log_msg("sup: control interface ready at %s", g_ep);

    while (!g_quit) {
        int op;
        if (ipc_server_accept(srv) != 0) break;  /* signal-interrupted */
        if (ipc_server_readline(srv, line, sizeof(line)) > 0) {
            op = ipc_dispatch(line, out, sizeof(out), svc_ops);
            ipc_server_write(srv, out);
            if (op == IPC_OP_QUIT) {
                ipc_server_closeconn(srv);
                break;
            }
        }
        ipc_server_closeconn(srv);
    }
    g_quit = 1;
    ipc_server_destroy(srv);
    log_msg("sup: control interface closed");
}

int main(int argc, char **argv)
{
    const char *conf = NULL;
    int opt, daemon_mode = 0;
    char exedir[4096];
    char chosen[4608];

    while ((opt = getopt(argc, argv, "c:ds:hV")) != -1) {
        switch (opt) {
        case 'c': conf = optarg; break;
        case 's':
            strncpy(g_ep, optarg, sizeof(g_ep) - 1);
            g_ep[sizeof(g_ep) - 1] = '\0';
            cli_set_endpoint(g_ep);
            break;
        case 'd': daemon_mode = 1; break;
        case 'V':
            printf("AppBootLauncher %s\n", APPBOOT_VERSION);
            return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (conf) {
        strncpy(chosen, conf, sizeof(chosen) - 1);
        chosen[sizeof(chosen) - 1] = '\0';
    } else {
        /* Default: <exe dir>/../conf/startup.conf (release / installed
         * layout). Development builds live one level deeper
         * (build/debug, build/release), so also try
         * <exe dir>/../../conf/startup.conf. If neither exists, keep
         * the documented default path for the error message. */
        static const char *suffixes[] = {
            "/../conf/startup.conf",
            "/../../conf/startup.conf"
        };
        int k;

        exe_dir(argv[0], exedir, sizeof(exedir));
        chosen[0] = '\0';
        for (k = 0; k < 2; k++) {
            if (strlen(exedir) + strlen(suffixes[k]) + 1
                    < sizeof(chosen)) {
                strcpy(chosen, exedir);
                strcat(chosen, suffixes[k]);
                if (conf_readable(chosen)) break;
            }
        }
        if (!chosen[0] || !conf_readable(chosen)) {
            if (strlen(exedir) + strlen(suffixes[0]) + 1
                    >= sizeof(chosen)) {
                log_msg("executable path too long to derive config path");
                return 2;
            }
            strcpy(chosen, exedir);
            strcat(chosen, suffixes[0]);
        }
    }

    if (optind < argc) {
        /* management / CLI mode: inspect or edit the configuration file */
        char **sub = argv + optind;
        int    nsub = argc - optind;
#ifdef HAVE_GUI
        if (strcmp(sub[0], "gui") == 0)
            return gui_main(chosen, run_all);
#endif
        if (strcmp(sub[0], "serve") == 0) {
            /* resident supervisor: boot everything, then keep serving
             * the control interface (used by systemd / SysV / desktop) */
            if (daemon_mode) daemonize();
            log_msg("posix supervisor " APPBOOT_VERSION
                    ": configuration file: %s", chosen);
            if (sup_load(chosen) != 0) return 1;
            sup_boot();
            serve_loop();
            return 0;
        }
        if (!cli_is_known(sub[0])) {
            log_msg("unknown command '%s'", sub[0]);
            usage(argv[0]);
            return 2;
        }
        return cli_main(nsub, sub, argv[0], chosen, run_all);
    }

    /* no subcommand: with a graphical session open the configuration GUI;
     * headless boot invocations (systemd / SysV / rc.local) keep the
     * original behaviour and just start the applications */
#ifdef HAVE_GUI
    if (!daemon_mode && have_graphical_session())
        return gui_main(chosen, run_all);
#endif

    if (daemon_mode) daemonize();

    log_msg("posix launcher " APPBOOT_VERSION ": configuration file: %s",
            chosen);
    return run_all(chosen) == 0 ? 0 : 1;
}
