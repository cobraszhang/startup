/* posix_main.c - boot-time application launcher for POSIX systems.
 *
 * Intended to be invoked once at boot by init (inittab / rc.local /
 * SysV init.d script) or by a systemd unit. It reads a configuration
 * file, topologically sorts the applications by their "deps" entries
 * and starts them in dependency order with fork + /bin/sh -c.
 *
 * Usage:
 *   startup [-c config-file] [-d]                  boot the applications
 *   startup [-c config-file] <subcommand> [args]   manage the configuration
 *     -c FILE   configuration to read (default: <exe dir>/../conf/startup.conf,
 *               resolved from /proc/self/exe, falling back to argv[0])
 *     -d        detach into the background (double fork + setsid)
 *   Run "startup help" for the management subcommands
 *   (list/order/check/run/add/rm/rename/set/dep/gui).
 *   With no subcommand: when a graphical session (GNOME, or any
 *   X11/Wayland display such as WSLg) is detected the GTK configuration
 *   window opens instead; headless boot sessions (systemd/SysV) keep the
 *   previous behaviour. The GUI is compiled in only when gtk+-3.0
 *   development files are available at build time.
 *
 * Portable C89 + POSIX.1 (fork, exec, setsid, nanosleep, readlink).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>

#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "cli.h"
#include "posix_app.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-c config-file] [-d] [subcommand ...]\n"
        "  default config: <exe dir>/../conf/startup.conf\n"
        "  -c FILE  configuration file (overrides the default)\n"
        "  -d       detach into the background (boot mode only)\n"
        "  (no subcommand) opens the configuration GUI when a graphical\n"
        "  session is detected; otherwise boots the applications\n",
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

int main(int argc, char **argv)
{
    const char *conf = NULL;
    int opt, daemon_mode = 0;
    char exedir[4096];
    char chosen[4608];

    while ((opt = getopt(argc, argv, "c:dh")) != -1) {
        switch (opt) {
        case 'c': conf = optarg; break;
        case 'd': daemon_mode = 1; break;
        default:  usage(argv[0]); return 2;
        }
    }

    if (conf) {
        strncpy(chosen, conf, sizeof(chosen) - 1);
        chosen[sizeof(chosen) - 1] = '\0';
    } else {
        /* default: <exe dir>/../conf/startup.conf */
        exe_dir(argv[0], exedir, sizeof(exedir));
        if (strlen(exedir) + strlen("/../conf/startup.conf") + 1
                >= sizeof(chosen)) {
            log_msg("executable path too long to derive config path");
            return 2;
        }
        strcpy(chosen, exedir);
        strcat(chosen, "/../conf/startup.conf");
    }

    if (optind < argc) {
        /* management / CLI mode: inspect or edit the configuration file */
        char **sub = argv + optind;
        int    nsub = argc - optind;
#ifdef HAVE_GUI
        if (strcmp(sub[0], "gui") == 0)
            return gui_main(chosen, run_all);
#endif
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

    log_msg("posix launcher: configuration file: %s", chosen);
    return run_all(chosen) == 0 ? 0 : 1;
}
