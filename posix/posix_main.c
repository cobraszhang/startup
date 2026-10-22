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
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>

#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "cli.h"
#include "state.h"
#include "version.h"
#include "ipc.h"
#include "posix_app.h"

static void usage(const char *prog)
{
    fprintf(stderr, "startup %s\n", APPBOOT_VERSION);
    fprintf(stderr,
        "usage: %s [-c config-file] [-s endpoint] [-d] [-V] [subcommand ...]\n"
        "  default config: <exe dir>/../conf/startup.conf\n"
        "  -c FILE  configuration file (overrides the default)\n"
        "  -s NAME  control endpoint for 'serve' and 'app' subcommands\n"
        "           (default /tmp/bootlauncher.ctl, env BOOTLAUNCHER_CTL)\n"
        "  -d       detach into the background (boot or serve mode)\n",
        prog);
    fprintf(stderr,
        "  -V       print version and exit\n"
        "  (no subcommand) opens the configuration GUI when a graphical\n"
        "  session is detected; otherwise boots the applications\n"
        "  serve     boot + stay resident as the system-side control server\n"
        "  user      stay resident as the USER-side control server:\n"
        "            waits until the system side finished, then starts\n"
        "            only application-class entries (run this from the\n"
        "            desktop session autostart, with -d or '&')\n");
    fprintf(stderr,
        "  install    register BOTH sides, adding only what is missing:\n"
        "             this user's autostart entry (XDG autostart desktop\n"
        "             file) and the system side (systemd unit when systemd\n"
        "             runs, else /etc/rc.local; via sudo -n if not root)\n"
        "  uninstall  remove both registrations again\n"
        "  install-system / uninstall-system\n"
        "             manage the system-side registration alone (root)\n");
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

/* ------------------------------------------------------------------ */
/* execution-time interpreter resolution (documented in posix_app.h)   */
/* ------------------------------------------------------------------ */

static void lower_copy(char *dst, size_t dstsz, const char *src)
{
    size_t i;
    for (i = 0; i + 1 < dstsz && src[i]; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    dst[i] = '\0';
}

static void passthrough(const char *stored, char *out, size_t outsz)
{
    strncpy(out, stored, outsz - 1);
    out[outsz - 1] = '\0';
}

/* map a file extension to the interpreter that can run it */
static const char *interp_for(const char *path)
{
    static const char *tab[][2] = {
        { "sh",   "/bin/sh"   }, { "bash", "/bin/bash" },
        { "py",   "python3"   }, { "pl",   "perl"      },
        { "rb",   "ruby"      }, { "js",   "node"      },
        { "lua",  "lua"       }, { NULL,   NULL        }
    };
    char        ext[16];
    const char *dot = strrchr(path, '.');
    int         i;

    if (!dot) return NULL;
    lower_copy(ext, sizeof(ext), dot + 1);
    for (i = 0; tab[i][0]; i++)
        if (strcmp(ext, tab[i][0]) == 0) return tab[i][1];
    return NULL;
}

/* locate an interpreter (PATH search); 1 = available, 0 = not */
static int find_interp(const char *name, char *out, size_t outsz)
{
    char        full[PATH_MAX];
    const char *pathenv = getenv("PATH");
    const char *p, *q;

    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            if (strlen(name) >= outsz) return 0;
            strcpy(out, name);
            return 1;
        }
        return 0;
    }
    if (!pathenv) pathenv = "/usr/local/bin:/usr/bin:/bin";
    p = pathenv;
    while (*p) {
        q = strchr(p, ':');
        if (!q) q = p + strlen(p);
        if (q == p)
            strncpy(full, name, sizeof(full) - 1);      /* empty = cwd */
        else
            sprintf(full, "%.*s/%s", (int)(q - p), p, name);
        full[sizeof(full) - 1] = '\0';
        if (access(full, X_OK) == 0) {
            if (strlen(full) >= outsz) return 0;
            strcpy(out, full);
            return 1;
        }
        p = q;
        if (*p == ':') p++;
    }
    return 0;
}

int app_build_cmdline(const char *stored, char *out, size_t outsz)
{
    char        path[PATH_MAX];
    char        interp[256];
    const char *p = stored;
    size_t      i;

    /* only a lone path (quoted or not) is interpreted; anything with
     * extra arguments is treated as a full command line */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        i = 0;
        while (*p && *p != '"' && i + 1 < sizeof(path)) path[i++] = *p++;
        if (*p != '"') { passthrough(stored, out, outsz); return 0; }
        path[i] = '\0';
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) { passthrough(stored, out, outsz); return 0; }
    } else {
        if (!*p || strpbrk(p, " \t\"'`$&;|<>()")) {
            passthrough(stored, out, outsz);
            return 0;
        }
        strncpy(path, p, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    /* an executable file runs as stored (kernel honors the shebang) */
    if (access(path, X_OK) == 0) {
        passthrough(stored, out, outsz);
        return 0;
    }
    /* nonexistent path: untouched (shell commands, builtins, ...) */
    if (access(path, F_OK) != 0) {
        passthrough(stored, out, outsz);
        return 0;
    }

    /* existing but not executable: prepend the interpreter for its
     * extension, if that runtime is actually installed */
    {
        const char *want = interp_for(path);
        if (!want || !find_interp(want, interp, sizeof(interp)))
            return -1;
        if (strlen(interp) + strlen(path) + 6 >= outsz) return -1;
        sprintf(out, "%s \"%s\"", interp, path);
        return 0;
    }
}

/* child process after fork: per-app working directory and
 * stdout/stderr capture, before the exec of the shell command */
static void child_setup(const app_t *a)
{
    if (a->cwd && chdir(a->cwd) != 0) {
        fprintf(stderr, "chdir '%s' failed for '%s'\n", a->cwd, a->name);
        _exit(127);
    }
    if (a->output) {
        int fd = open(a->output, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
    }
}

/* start one application through the shell; parent does not wait */
static int launch_app(const app_t *a)
{
    char  cmdline[4096];
    pid_t p;

    if (app_build_cmdline(a->cmd, cmdline, sizeof(cmdline)) != 0) {
        log_msg("launch: no interpreter available for '%s' (cmd='%s')",
                a->name, a->cmd);
        return -1;
    }
    p = fork();

    if (p < 0) {
        log_msg("launch: fork failed for '%s'", a->name);
        return -1;
    }
    if (p == 0) {
        child_setup(a);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        /* only reached when exec failed */
        fprintf(stderr, "exec '%s' failed\n", cmdline);
        _exit(127);
    }
    log_msg("launch: started '%s' pid=%ld", a->name, (long)p);
    st_record(a->name, ST_RUNNING);
    return 0;
}

/* non-zero on the no-subcommand boot path: run_all then honours the
 * per-app load = start/stop/auto setting (the CLI 'run' verb and the
 * GUI "start all" action are explicit user actions and start
 * everything) */
static int g_boot_mode = 0;

int run_all(const char *confpath)
{
    app_t *apps = NULL;
    int   *order = NULL;
    int    n = 0, rc, i, cnt = 0;

    if (cfg_load(confpath, &apps, &n) != 0) return 1;
    rc = topo_sort(apps, n, TOPO_ALL, &order, &cnt);
    if (rc != 0) {
        log_msg("startup aborted: dependency ordering failed (code %d)", rc);
        cfg_free(apps, n);
        return 1;
    }

    log_msg("startup: %d application(s), order:", n);
    for (i = 0; i < cnt; i++)
        log_msg("startup:   %d. %s", i + 1, apps[order[i]].name);

    for (i = 0; i < cnt; i++) {
        const app_t *a = &apps[order[i]];
        if (g_boot_mode && app_load(a) != LOAD_START) {
            int go = (app_load(a) == LOAD_AUTO)
                   ? (st_last(a->name) == ST_RUNNING) : 0;
            if (!go) {
                log_msg("startup: NOT starting '%s' at boot (load=%s%s)",
                        a->name,
                        app_load(a) == LOAD_STOP ? "stop" : "auto",
                        app_load(a) == LOAD_STOP ? ""
                                : ", last recorded state was not running");
                continue;
            }
            log_msg("startup: starting '%s' at boot (load=auto, last "
                    "recorded state was running)", a->name);
        }
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
static int     g_kind = KIND_SYSTEM;   /* class this process supervises */

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
    char  cmdline[4096];
    pid_t p;

    if (app_build_cmdline(g_sapps[i].cmd, cmdline, sizeof(cmdline)) != 0) {
        log_msg("sup: no interpreter available for '%s' (cmd='%s')",
                g_sapps[i].name, g_sapps[i].cmd);
        return -1;
    }
    p = fork();

    if (p < 0) {
        log_msg("sup: fork failed for '%s'", g_sapps[i].name);
        return -1;
    }
    if (p == 0) {
        child_setup(&g_sapps[i]);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        fprintf(stderr, "exec '%s' failed\n", cmdline);
        _exit(127);
    }
    g_srt[i].pid   = p;
    g_srt[i].state = 1;
    g_srt[i].code  = -1;
    log_msg("sup: started '%s' pid=%ld", g_sapps[i].name, (long)p);
    st_record(g_sapps[i].name, ST_RUNNING);
    return 0;
}

static void sup_boot(void)
{
    int *order = NULL;
    int  i, rc, cnt = 0;

    rc = topo_sort(g_sapps, g_sn, g_kind, &order, &cnt);
    if (rc != 0) {
        log_msg("startup aborted: dependency ordering failed (code %d)", rc);
        return;
    }
    log_msg("startup:%s %d application(s), order:",
            g_kind == KIND_APP ? " (user applications)" : "", cnt);
    for (i = 0; i < cnt; i++)
        log_msg("startup:   %d. %s", i + 1, g_sapps[order[i]].name);

    for (i = 0; i < cnt && !g_quit; i++) {
        int idx = order[i];
        int lm  = app_load(&g_sapps[idx]);
        if (lm != LOAD_START) {
            int go = (lm == LOAD_AUTO)
                   ? (st_last(g_sapps[idx].name) == ST_RUNNING) : 0;
            if (!go) {
                log_msg("startup: NOT starting '%s' at boot (load=%s%s)",
                        g_sapps[idx].name,
                        lm == LOAD_STOP ? "stop" : "auto",
                        lm == LOAD_STOP ? ""
                                : ", last recorded state was not running");
                continue;
            }
            log_msg("startup: starting '%s' at boot (load=auto, last "
                    "recorded state was running)", g_sapps[idx].name);
        }
        sup_launch(idx);
        if (g_sapps[idx].wait_ms > 0)
            sleep_ms((unsigned)g_sapps[idx].wait_ms);
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
            st_record(g_sapps[i].name, ST_STOPPED);
        } else if (r < 0) {
            /* ECHILD: never waitable (already reaped elsewhere) */
            g_srt[i].state = 2;
            st_record(g_sapps[i].name, ST_STOPPED);
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
    st_record(g_sapps[i].name, ST_STOPPED);
}

/* find a name-target for control verbs; an app is only controllable by
 * the supervisor of its startup class */
static int sup_find(const char *name, char *out, size_t sz)
{
    int idx = cfg_index(g_sapps, g_sn, name);
    if (idx < 0) {
        rep(out, sz, "ERR unknown app '%s'\n", name);
        return -1;
    }
    if (app_kind(&g_sapps[idx]) != g_kind) {
        int want = app_kind(&g_sapps[idx]);
        rep(out, sz, "ERR '%s' is a %s-class app; ask the %s supervisor\n",
            name, want == KIND_APP ? "application" : "system",
            want == KIND_APP ? "user" : "system-side");
        return -1;
    }
    return idx;
}

static int svc_ops(int op, const char *name, char *out, size_t sz)
{
    int idx;

    out[0] = '\0';
    switch (op) {
    case IPC_OP_PING:
        rep(out, sz, "OK PONG startup %s\n", APPBOOT_VERSION);
        break;
    case IPC_OP_LIST:
        rep(out, sz, "OK configured applications:\n");
        for (idx = 0; idx < g_sn; idx++) {
            if (app_kind(&g_sapps[idx]) != g_kind) continue;
            rep(out, sz, "  %-20s cmd=%s\n",
                g_sapps[idx].name, g_sapps[idx].cmd);
        }
        break;
    case IPC_OP_STATUS:
        if (*name) {
            char st[16];
            if ((idx = sup_find(name, out, sz)) < 0) break;
            state_name(rt_state(idx), st, sizeof(st));
            rep(out, sz, "OK status %s:\n  %s %s pid=%ld\n",
                name, g_sapps[idx].name, st, (long)g_srt[idx].pid);
        } else {
            rep(out, sz, "OK status:\n");
            for (idx = 0; idx < g_sn; idx++) {
                char st[16];
                if (app_kind(&g_sapps[idx]) != g_kind) continue;
                state_name(rt_state(idx), st, sizeof(st));
                rep(out, sz, "  %-20s %s pid=%ld\n",
                    g_sapps[idx].name, st, (long)g_srt[idx].pid);
            }
        }
        break;
    case IPC_OP_START:
        if ((idx = sup_find(name, out, sz)) < 0)  break;
        if (rt_state(idx) == 1)   rep(out, sz, "ERR '%s' already running (pid=%ld)\n",
                                        name, (long)g_srt[idx].pid);
        else if (sup_launch(idx) != 0) rep(out, sz, "ERR launch failed for '%s'\n", name);
        else                           rep(out, sz, "OK started '%s' pid=%ld\n",
                                            name, (long)g_srt[idx].pid);
        break;
    case IPC_OP_STOP:
        if ((idx = sup_find(name, out, sz)) < 0) break;
        if (rt_state(idx) != 1) rep(out, sz, "ERR '%s' not running\n", name);
        else {
            sup_stop(idx);
            log_msg("sup: stopped '%s' pid=%ld", g_sapps[idx].name,
                    (long)g_srt[idx].pid);
            rep(out, sz, "OK stopped '%s'\n", name);
        }
        break;
    case IPC_OP_RESTART:
        if ((idx = sup_find(name, out, sz)) < 0) break;
        if (rt_state(idx) == 1) sup_stop(idx);
        if (sup_launch(idx) != 0)
            rep(out, sz, "ERR restart failed for '%s'\n", name);
        else
            rep(out, sz, "OK restarted '%s' pid=%ld\n",
                name, (long)g_srt[idx].pid);
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

/* ------------------------------------------------------------------ */
/* install / uninstall: register BOTH startup sides (POSIX counterpart  */
/* of the Win32 service + Run-value model). The user side is this      */
/* user's XDG autostart desktop file; the system side is a systemd     */
/* unit when systemd runs, else an /etc/rc.local line (WSL without    */
/* systemd, container, plain init). Anything already registered is     */
/* detected and left alone - install never duplicates, the entry names */
/* are fixed (startup.service / startup-user.desktop).                 */
/* ------------------------------------------------------------------ */

#define UNIT_NAME    "startup.service"
#define UNIT_PATH    "/etc/systemd/system/startup.service"
#define RCLOCAL_PATH "/etc/rc.local"
#define SERVE_MARK   "-d serve"

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int file_exists(const char *p)
{
    return access(p, F_OK) == 0;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char  line[2048];
    int   found = 0;

    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

static void exe_absolute(const char *argv0, char *out, size_t outsz)
{
    ssize_t n = readlink("/proc/self/exe", out, outsz - 1);
    if (n > 0) {
        out[n] = '\0';
        return;
    }
    strncpy(out, argv0, outsz - 1);
    out[outsz - 1] = '\0';
}

/* <XDG_CONFIG_HOME or HOME/.config> shared base for both user-side
 * registrations (XDG autostart file, systemd user unit) */
static int config_home(char *out, size_t outsz)
{
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    out[0] = '\0';
    if (xdg && *xdg) {
        strncpy(out, xdg, outsz - 1);
    } else if (home && *home) {
        strncpy(out, home, outsz - 1);
        strncat(out, "/.config", outsz - strlen(out) - 1);
    } else {
        return -1;
    }
    out[outsz - 1] = '\0';
    return 0;
}

static int autostart_file(char *out, size_t outsz)
{
    if (config_home(out, outsz) != 0) return -1;
    if (strlen(out) + 40 >= outsz) return -1;
    strcat(out, "/autostart/startup-user.desktop");
    return 0;
}

/* With systemd running an XDG autostart file is only consumed by a
 * desktop session manager - a headless/WSL systemd box has none, so
 * the user side registers as a systemd USER unit instead (effective
 * at login, enable-able without root; `loginctl enable-linger` can
 * start it before any login). */
static int userunit_file(char *out, size_t outsz)
{
    if (config_home(out, outsz) != 0) return -1;
    if (strlen(out) + 40 >= outsz) return -1;
    strcat(out, "/systemd/user/startup-user.service");
    return 0;
}

static void mkdir_ignoring_exists(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        log_msg("install: cannot create directory %s", path);
    }
}

/* mkdir -p for a path whose parents are user-owned (best effort) */
static int mkdir_nested(const char *path)
{
    char  buf[4096];
    char *p;

    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int install_user_autostart(const char *exeabs)
{
    char  file[4096];
    char  dir[4096];
    char *slash;
    FILE *f;
    int   unit = is_dir("/run/systemd/system");

    if (unit) {
        if (userunit_file(file, sizeof(file)) != 0) {
            log_msg("install: cannot determine the user unit location "
                    "(set XDG_CONFIG_HOME or HOME)");
            return 1;
        }
    } else if (autostart_file(file, sizeof(file)) != 0) {
        log_msg("install: cannot determine the autostart location "
                "(set XDG_CONFIG_HOME or HOME)");
        return 1;
    }
    if (file_exists(file)) {
        log_msg("install: user-side entry already present, left "
                "unchanged: %s", file);
        return 0;
    }
    strcpy(dir, file);
    slash = strrchr(dir, '/');
    if (!slash) return 1;
    *slash = '\0';
    if (unit) {
        if (mkdir_nested(dir) != 0) {
            log_msg("install: cannot create %s", dir);
            return 1;
        }
    } else {
        char *slash2 = strrchr(dir, '/');
        if (!slash2) return 1;
        *slash2 = '\0';                 /* .../.config */
        mkdir_ignoring_exists(dir);
        *slash2 = '/';
        mkdir_ignoring_exists(dir);
    }
    f = fopen(file, "w");
    if (!f) {
        log_msg("install: cannot write %s", file);
        return 1;
    }
    if (unit) {
        fprintf(f,
            "[Unit]\n"
            "Description=startup user supervisor (application-class "
            "entries at login)\n\n"
            "[Service]\n"
            "ExecStart=\"%s\" -d user\n"
            "Restart=on-failure\n\n"
            "[Install]\n"
            "WantedBy=default.target\n",
            exeabs);
        fclose(f);
        if (system("systemctl --user daemon-reload >/dev/null 2>&1") != 0)
            log_msg("install: NOTE systemctl --user daemon-reload "
                    "unavailable (no user session bus?)");
        if (system("systemctl --user enable startup-user.service "
                   ">/dev/null 2>&1") != 0)
            log_msg("install: NOTE systemctl --user enable failed; "
                    "enable manually: systemctl --user enable "
                    "startup-user.service");
        log_msg("install: user systemd unit registered and enabled: %s "
                "(starts at login; for boot without login run: "
                "loginctl enable-linger \"$USER\")", file);
    } else {
        fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=startup user supervisor\n"
            "Comment=start application-class entries at logon\n"
            "Exec=\"%s\" -d user\n"
            "Terminal=false\n"
            "X-GNOME-Autostart-enabled=true\n",
            exeabs);
        fclose(f);
        chmod(file, 0755);
        log_msg("install: user autostart registered: %s -> \"%s\" -d user",
                file, exeabs);
    }
    return 0;
}

static int uninstall_user_autostart(void)
{
    char file[4096];
    int  removed = 0;
    int  rc = 0;

    /* remove either registration style (user unit and/or autostart
     * file), whichever exists - a machine may have been registered
     * while systemd was off/on */
    if (userunit_file(file, sizeof(file)) == 0 && file_exists(file)) {
        if (system("systemctl --user disable startup-user.service "
                   ">/dev/null 2>&1") != 0)
            log_msg("uninstall: NOTE systemctl --user disable failed");
        if (unlink(file) != 0) {
            log_msg("uninstall: could not remove %s", file);
            rc = 1;
        } else {
            log_msg("uninstall: user systemd unit removed: %s", file);
            removed = 1;
        }
        if (system("systemctl --user daemon-reload >/dev/null 2>&1") != 0)
            log_msg("uninstall: NOTE systemctl --user daemon-reload "
                    "unavailable");
    }
    if (autostart_file(file, sizeof(file)) == 0 && file_exists(file)) {
        if (unlink(file) != 0) {
            log_msg("uninstall: could not remove %s", file);
            rc = 1;
        } else {
            log_msg("uninstall: user autostart entry removed: %s", file);
            removed = 1;
        }
    }
    if (!removed && rc == 0) {
        log_msg("uninstall: no user-side registration found");
    }
    return rc;
}

static int install_system(const char *exeabs, const char *conffile)
{
    FILE *f;

    if (is_dir("/run/systemd/system")) {
        if (file_exists(UNIT_PATH)) {
            log_msg("install: systemd unit already installed, left "
                    "alone: %s", UNIT_PATH);
            return 0;
        }
        if (geteuid() != 0) {
            log_msg("install: installing the systemd unit needs root");
            return 1;
        }
        f = fopen(UNIT_PATH, "w");
        if (!f) {
            log_msg("install: cannot write %s", UNIT_PATH);
            return 1;
        }
        fprintf(f,
            "[Unit]\n"
            "Description=startup boot-time launcher (system side)\n"
            "After=network.target\n\n"
            "[Service]\n"
            "ExecStart=\"%s\" -d serve\n"
            "Restart=on-failure\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n", exeabs);
        fclose(f);
        if (system("systemctl daemon-reload >/dev/null 2>&1") != 0)
            log_msg("install: WARNING systemctl daemon-reload failed");
        if (system("systemctl enable " UNIT_NAME " >/dev/null 2>&1") != 0)
            log_msg("install: WARNING systemctl enable failed");
        log_msg("install: systemd unit installed and enabled: %s",
                UNIT_PATH);
        return 0;
    }

    /* no systemd running: /etc/rc.local fallback */
    if (file_contains(RCLOCAL_PATH, SERVE_MARK)) {
        log_msg("install: " RCLOCAL_PATH " already runs the system-side "
                "supervisor, left unchanged");
        return 0;
    }
    if (geteuid() != 0) {
        log_msg("install: registering the system side in " RCLOCAL_PATH
                " needs root");
        return 1;
    }
    {
        int fresh = !file_exists(RCLOCAL_PATH);
        f = fopen(RCLOCAL_PATH, fresh ? "w" : "a");
        if (!f) {
            log_msg("install: cannot write " RCLOCAL_PATH);
            return 1;
        }
        if (fresh) {
            fprintf(f, "#!/bin/sh -e\n");    /* created: make it valid */
        } else {
            fputc('\n', f);                  /* append on its own line */
        }
        fprintf(f, "\"%s\" -d serve -c \"%s\"\n", exeabs, conffile);
        fclose(f);
    }
    chmod(RCLOCAL_PATH, 0755);
    log_msg("install: appended the system-side command to " RCLOCAL_PATH);
    return 0;
}

/* rewrite a file without the lines matching `drop` */
static int file_strip_lines(const char *path, const char *drop)
{
    char  tmp[4096];
    FILE *in, *out;
    char  line[2048];
    int   changed = 0;

    strcpy(tmp, path);
    strcat(tmp, ".startup-tmp");
    in = fopen(path, "r");
    if (!in) return -1;
    out = fopen(tmp, "w");
    if (!out) { fclose(in); return -1; }
    while (fgets(line, sizeof(line), in)) {
        if (strstr(line, drop)) { changed = 1; continue; }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (!changed) { unlink(tmp); return 0; }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 1;
}

static int uninstall_system(void)
{
    int r;

    if (is_dir("/run/systemd/system")) {
        if (!file_exists(UNIT_PATH)) {
            log_msg("uninstall: no systemd unit installed");
            return 0;
        }
        if (geteuid() != 0) {
            log_msg("uninstall: removing the systemd unit needs root");
            return 1;
        }
        if (system("systemctl disable " UNIT_NAME " >/dev/null 2>&1") != 0)
            log_msg("uninstall: WARNING systemctl disable failed");
        if (unlink(UNIT_PATH) != 0) {
            log_msg("uninstall: could not remove %s", UNIT_PATH);
            return 1;
        }
        if (system("systemctl daemon-reload >/dev/null 2>&1") != 0)
            log_msg("uninstall: WARNING systemctl daemon-reload failed");
        log_msg("uninstall: systemd unit removed: %s", UNIT_PATH);
        return 0;
    }

    if (!file_exists(RCLOCAL_PATH)) {
        log_msg("uninstall: nothing to remove (no systemd, no rc.local)");
        return 0;
    }
    if (!file_contains(RCLOCAL_PATH, SERVE_MARK)) {
        log_msg("uninstall: no launcher line in " RCLOCAL_PATH);
        return 0;
    }
    if (geteuid() != 0) {
        log_msg("uninstall: editing " RCLOCAL_PATH " needs root");
        return 1;
    }
    r = file_strip_lines(RCLOCAL_PATH, SERVE_MARK);
    if (r < 0) {
        log_msg("uninstall: could not rewrite " RCLOCAL_PATH);
        return 1;
    }
    log_msg("uninstall: removed the system-side line from " RCLOCAL_PATH);
    return 0;
}

/* one non-interactive sudo retry of the system-side half only; used by
 * install/uninstall when /etc could not be written as the user */
static int try_sudo(const char *sub, const char *conffile,
                    const char *argv0)
{
    char  self[PATH_MAX];
    char *args[7];

    exe_absolute(argv0, self, sizeof(self));
    args[0] = "sudo";
    args[1] = "-n";                    /* never prompt: fail fast */
    args[2] = self;
    args[3] = "-c";
    args[4] = (char *)conffile;
    args[5] = (char *)sub;
    args[6] = NULL;
    execvp("sudo", args);
    return 1;                          /* sudo not even installed */
}

/* User applications must never start before system services: wait
 * until the system-side supervisor's control endpoint exists - that
 * socket appears only once every system-class app has been launched.
 * BOOTLAUNCHER_WAITMAX overrides the bound in seconds (0 = no wait);
 * on timeout we proceed with a warning (the system side may simply
 * not be installed on this machine). */
static void wait_system_ready(void)
{
    char         ep[512];
    const char  *w;
    int          cap_s, waited_ms;

    w = getenv("BOOTLAUNCHER_WAITMAX");
    cap_s = (w && *w) ? atoi(w) : 60;
    ipc_default_endpoint(ep, sizeof(ep), KIND_SYSTEM);
    for (waited_ms = 0; waited_ms <= cap_s * 1000; waited_ms += 500) {
        struct stat sb;
        if (g_quit) return;
        if (stat(ep, &sb) == 0) return;   /* system side is up */
        sleep_ms(500);
    }
    log_msg("user: WARNING no system-side control endpoint at %s after "
            "%d s; starting anyway", ep, cap_s);
}

static void serve_loop(void)
{
    void *srv;
    char  line[IPC_LINE_MAX], out[8192];

    if (!g_ep[0]) ipc_default_endpoint(g_ep, sizeof(g_ep), g_kind);
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
            printf("startup %s\n", APPBOOT_VERSION);
            return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    /* the state log lives next to the runtime log; derive the exe
     * directory unconditionally for both conf and state defaults */
    exe_dir(argv[0], exedir, sizeof(exedir));

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

    /* state log for load = auto decisions (directory: the logs dir of
     * the release layout, created when permitted; when
     * BOOTLAUNCHER_LOG is set its own directory is used as-is) */
    {
        char logsdir[4608];
        if (strlen(exedir) + 10 < sizeof(logsdir)) {
            strcpy(logsdir, exedir);
            strcat(logsdir, "/../logs");
            (void)mkdir(logsdir, 0775);  /* exists / not writable: fine */
        }
    }
    st_init(getenv("BOOTLAUNCHER_LOG"), exedir);

    if (optind < argc) {
        /* management / CLI mode: inspect or edit the configuration file */
        char **sub = argv + optind;
        int    nsub = argc - optind;
#ifdef HAVE_GUI
        if (strcmp(sub[0], "gui") == 0)
            return gui_main(chosen, run_all);
#endif
        if (strcmp(sub[0], "serve") == 0) {
            /* resident system-side supervisor: boots the system-class
             * entries, then keeps serving the control interface
             * (used by systemd / SysV / desktop) */
            if (daemon_mode) daemonize();
            log_msg("startup supervisor " APPBOOT_VERSION
                    ": configuration file: %s", chosen);
            if (sup_load(chosen) != 0) return 1;
            sup_boot();
            serve_loop();
            return 0;
        }
        if (strcmp(sub[0], "user") == 0) {
            /* resident user-side supervisor: waits until the system
             * side has finished starting its apps, then starts and
             * supervises application-class entries only; run it from
             * the desktop session's autostart (see contrib/) */
            if (daemon_mode) daemonize();
            g_kind = KIND_APP;
            log_msg("startup user supervisor " APPBOOT_VERSION
                    ": configuration file: %s", chosen);
            wait_system_ready();
            if (sup_load(chosen) != 0) return 1;
            sup_boot();
            serve_loop();
            return 0;
        }
        if (strcmp(sub[0], "install") == 0) {
            /* both sides, analysed like the Win32 install: never touches
             * an existing registration, adds what is missing */
            char exeabs[PATH_MAX];
            int  rc_u, rc_s;
            exe_absolute(argv[0], exeabs, sizeof(exeabs));
            rc_u = install_user_autostart(exeabs);
            rc_s = install_system(exeabs, chosen);
            if (rc_s != 0 && geteuid() != 0) {
                log_msg("install: retrying the system side through "
                        "'sudo -n' (needs passwordless sudo)");
                try_sudo("install-system", chosen, argv[0]);
                log_msg("install: could not run sudo; finish with: "
                        "sudo %s install-system -c \"%s\"",
                        exeabs, chosen);
                rc_s = 1;
            }
            return (rc_u != 0 || rc_s != 0) ? 1 : 0;
        }
        if (strcmp(sub[0], "uninstall") == 0) {
            int rc_u = uninstall_user_autostart();
            int rc_s = uninstall_system();
            if (rc_s != 0 && geteuid() != 0) {
                log_msg("uninstall: retrying the system side through "
                        "'sudo -n' (needs passwordless sudo)");
                try_sudo("uninstall-system", chosen, argv[0]);
                log_msg("uninstall: could not run sudo; finish with: "
                        "sudo <program> uninstall-system");
                rc_s = 1;
            }
            return (rc_u != 0 || rc_s != 0) ? 1 : 0;
        }
        if (strcmp(sub[0], "install-system") == 0) {
            char exeabs[PATH_MAX];
            exe_absolute(argv[0], exeabs, sizeof(exeabs));
            return install_system(exeabs, chosen);
        }
        if (strcmp(sub[0], "uninstall-system") == 0)
            return uninstall_system();
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

    g_boot_mode = 1;      /* no-args boot honours load = start/stop/auto */
    log_msg("startup " APPBOOT_VERSION ": configuration file: %s",
            chosen);
    return run_all(chosen) == 0 ? 0 : 1;
}
