/* win32_service.c - boot-time application launcher as a Windows service.
 *
 * The service is registered with SERVICE_AUTO_START (binPath ends with
 * the "service" argument), so the SCM starts it at every boot. It reads
 * ..\conf\startup.conf relative to the executable, topologically sorts
 * the applications by their "deps" entries and launches them in
 * dependency order with CreateProcess.
 *
 * Usage:
 *   startup.exe                  open the configuration/status GUI window
 *   startup.exe service          run as a service (dispatched by the SCM)
 *   startup.exe install          create the service (auto start, needs admin)
 *   startup.exe uninstall        remove the service (needs admin)
 *   startup.exe start | stop     control the installed service
 *   startup.exe run | console    run the launcher interactively (debug)
 *   startup.exe help             management subcommands (list/order/add/...)
 *   -c <file>                    use an alternate configuration file
 *
 * Compiles with MSVC 6.0 (/TC) and MinGW-w64 gcc -std=c89. C89 only.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "cli.h"
#include "version.h"
#include "ipc.h"
#include "win32_app.h"

#define CONF_SUBPATH   "..\\conf\\startup.conf"
#define CMD_MAX        4096
#define START_WAIT_MS  3000   /* wait hint while starting / stopping */

/* Vista-era flag; absent from the VC6 SDK headers. Apps are started
 * without any console window (silently), which also keeps user-session
 * supervisors from littering the desktop with conhost windows. */
#ifndef CREATE_NO_WINDOW
#  define CREATE_NO_WINDOW 0x08000000
#endif

static SERVICE_STATUS_HANDLE g_ssh   = NULL;
static SERVICE_STATUS        g_status;
static HANDLE                g_stop  = NULL;   /* manual-reset stop event */

/* ------------------------------------------------------------------ */
/* service control manager plumbing                                     */
/* ------------------------------------------------------------------ */

static void report_status(DWORD state, DWORD wait_hint)
{
    static DWORD checkpoint = 1;

    g_status.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted =
        (state == SERVICE_START_PENDING)
            ? 0
            : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    g_status.dwWin32ExitCode = NO_ERROR;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    g_status.dwWaitHint = wait_hint;

    if (g_ssh) SetServiceStatus(g_ssh, &g_status);
}

static void WINAPI ctrl_handler(DWORD ctrl)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        report_status(SERVICE_STOP_PENDING, START_WAIT_MS);
        if (g_stop) SetEvent(g_stop);
        /* Applications already launched are left running on purpose. */
    }
    /* SERVICE_CONTROL_INTERROGATE: default handling reports current state. */
}

/* ------------------------------------------------------------------ */
/* launching                                                            */
/* ------------------------------------------------------------------ */

static int launch_app(const app_t *a)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[CMD_MAX];

    strncpy(cmdline, a->cmd, sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                        NULL, NULL, &si, &pi)) {
        log_msg("launch: FAILED '%s' (cmd='%s') err=%lu",
                a->name, a->cmd, (unsigned long)GetLastError());
        return -1;
    }
    log_msg("launch: started '%s' pid=%lu",
            a->name, (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

/* Load config, compute dependency order, start everything.
 * Returns 0 on success, non-zero on configuration/order failure. */
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
        if (a->wait_ms > 0) {
            /* interruptible pause so a stop request is honoured quickly */
            if (g_stop && WaitForSingleObject(g_stop, (DWORD)a->wait_ms)
                              == WAIT_OBJECT_0) {
                log_msg("startup: stop requested, remaining apps not started");
                break;
            }
        }
    }

    free(order);
    cfg_free(apps, n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* supervisor: resident control interface for user processes           */
/*                                                                     */
/* Started by the SCM ("service") or in the foreground ("serve").      */
/* Loads the model, starts the apps in dependency order but KEEPS the  */
/* process handles, and serves the line protocol from src/ipc.h on a   */
/* named pipe so user processes can START/STOP/RESTART/STATUS them.    */
/* ------------------------------------------------------------------ */

typedef struct {
    HANDLE h;      /* process handle while running, else NULL        */
    DWORD  pid;
    int    state;  /* 0 not started, 1 running, 2 stopped/exited     */
} rt_t;

static app_t        *g_sapps;                 /* kept for the run     */
static int           g_sn;
static rt_t         *g_srt;
static char          g_ep[512];
static volatile long g_supquit;

static void ep_ensure(void)
{
    if (!g_ep[0]) ipc_default_endpoint(g_ep, sizeof(g_ep));
}

/* append one formatted line to the reply buffer */
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
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[CMD_MAX];

    strncpy(cmdline, g_sapps[i].cmd, sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                        NULL, NULL, &si, &pi)) {
        log_msg("sup: start '%s' failed (cmd='%s') err=%lu",
                g_sapps[i].name, g_sapps[i].cmd,
                (unsigned long)GetLastError());
        return -1;
    }
    CloseHandle(pi.hThread);
    g_srt[i].h     = pi.hProcess;
    g_srt[i].pid   = pi.dwProcessId;
    g_srt[i].state = 1;
    log_msg("sup: started '%s' pid=%lu",
            g_sapps[i].name, (unsigned long)pi.dwProcessId);
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

    for (i = 0; i < g_sn; i++) {
        int idx = order[i];
        sup_launch(idx);
        if (g_sapps[idx].wait_ms > 0 && g_stop &&
            WaitForSingleObject(g_stop, (DWORD)g_sapps[idx].wait_ms)
                == WAIT_OBJECT_0) {
            log_msg("startup: stop requested, remaining apps not started");
            break;
        }
    }
    free(order);
}

/* refresh and return the state of entry i (detects natural exit) */
static int rt_state(int i)
{
    if (g_srt[i].state == 1) {
        DWORD code = 0;
        if (!g_srt[i].h ||
            (!GetExitCodeProcess(g_srt[i].h, &code) ||
             code != STILL_ACTIVE)) {
            if (g_srt[i].h) CloseHandle(g_srt[i].h);
            g_srt[i].h     = NULL;
            g_srt[i].state = 2;
        }
    }
    return g_srt[i].state;
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
            rep(out, sz, "OK status %s:\n  %s %s pid=%lu\n",
                name, g_sapps[idx].name, st,
                (unsigned long)g_srt[idx].pid);
        } else {
            rep(out, sz, "OK status:\n");
            for (idx = 0; idx < g_sn; idx++) {
                char st[16];
                state_name(rt_state(idx), st, sizeof(st));
                rep(out, sz, "  %-20s %s pid=%lu\n",
                    g_sapps[idx].name, st,
                    (unsigned long)g_srt[idx].pid);
            }
        }
        break;
    case IPC_OP_START:
        idx = cfg_index(g_sapps, g_sn, name);
        if (idx < 0)                    rep(out, sz, "ERR unknown app '%s'\n", name);
        else if (rt_state(idx) == 1)    rep(out, sz, "ERR '%s' already running (pid=%lu)\n",
                                            name, (unsigned long)g_srt[idx].pid);
        else if (sup_launch(idx) != 0)  rep(out, sz, "ERR launch failed for '%s'\n", name);
        else                            rep(out, sz, "OK started '%s' pid=%lu\n",
                                            name, (unsigned long)g_srt[idx].pid);
        break;
    case IPC_OP_STOP:
        idx = cfg_index(g_sapps, g_sn, name);
        if (idx < 0)                 rep(out, sz, "ERR unknown app '%s'\n", name);
        else if (rt_state(idx) != 1) rep(out, sz, "ERR '%s' not running\n", name);
        else {
            TerminateProcess(g_srt[idx].h, 0);
            CloseHandle(g_srt[idx].h);
            g_srt[idx].h     = NULL;
            g_srt[idx].state = 2;
            log_msg("sup: stopped '%s' pid=%lu", g_sapps[idx].name,
                    (unsigned long)g_srt[idx].pid);
            rep(out, sz, "OK stopped '%s'\n", name);
        }
        break;
    case IPC_OP_RESTART:
        idx = cfg_index(g_sapps, g_sn, name);
        if (idx < 0)                rep(out, sz, "ERR unknown app '%s'\n", name);
        else {
            if (rt_state(idx) == 1) {
                TerminateProcess(g_srt[idx].h, 0);
                CloseHandle(g_srt[idx].h);
                g_srt[idx].h     = NULL;
                g_srt[idx].state = 2;
            }
            if (sup_launch(idx) != 0)
                rep(out, sz, "ERR restart failed for '%s'\n", name);
            else
                rep(out, sz, "OK restarted '%s' pid=%lu\n",
                    name, (unsigned long)g_srt[idx].pid);
        }
        break;
    case IPC_OP_QUIT:
        g_supquit = 1;
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

    ep_ensure();
    srv = ipc_server_open(g_ep);
    if (!srv) {
        log_msg("sup: cannot create control endpoint '%s'", g_ep);
        return;
    }
    log_msg("sup: control interface ready at %s", g_ep);

    while (!g_supquit &&
           !(g_stop && WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0)) {
        int op;
        if (ipc_server_accept(srv) != 0) break;
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
    g_supquit = 1;
    ipc_server_destroy(srv);
    log_msg("sup: control interface closed");
}

static DWORD WINAPI serve_thr(LPVOID p)
{
    (void)p;
    serve_loop();
    return 0;
}

/* ------------------------------------------------------------------ */
/* service entry                                                        */
/* ------------------------------------------------------------------ */

/* Default configuration: prefer <exe dir>\..\conf\startup.conf
 * (release layout). If it does not exist, try
 * <exe dir>\..\..\conf\startup.conf so development builds placed in
 * build\debug or build\release resolve the repository conf\ directory.
 * If neither exists, report the standard release-layout path. */
#define CONF_SUBPATH   "..\\conf\\startup.conf"
#define CONF_SUBPATH2  "..\\..\\conf\\startup.conf"

static int conf_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

void default_conf_path(const char *exepath, char *out, size_t outsz)
{
    char  base[1024];
    char  cand[1120];
    char *slash;

    strncpy(base, exepath, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    slash = strrchr(base, '\\');
    if (!slash) slash = strrchr(base, '/');
    if (slash) *slash = '\0';

    if (strlen(base) + 1 + strlen(CONF_SUBPATH) < sizeof(cand)) {
        sprintf(cand, "%s\\%s", base, CONF_SUBPATH);
        if (conf_file_exists(cand)) {
            strncpy(out, cand, outsz - 1);
            out[outsz - 1] = '\0';
            return;
        }
        if (strlen(base) + 1 + strlen(CONF_SUBPATH2) < sizeof(cand)) {
            sprintf(cand, "%s\\%s", base, CONF_SUBPATH2);
            if (conf_file_exists(cand)) {
                strncpy(out, cand, outsz - 1);
                out[outsz - 1] = '\0';
                return;
            }
        }
    }
    /* nothing present: keep the documented default for error messages */
    strncpy(out, exepath, outsz - 1);
    out[outsz - 1] = '\0';
    slash = strrchr(out, '\\');
    if (!slash) slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    if (strlen(out) + 1 + strlen(CONF_SUBPATH) < outsz) {
        strcat(out, "\\");
        strcat(out, CONF_SUBPATH);
    } else {
        strncpy(out, CONF_SUBPATH, outsz - 1);
        out[outsz - 1] = '\0';
    }
}

static void WINAPI service_main(DWORD argc, char **argv)
{
    char conf[MAX_PATH * 2];
    char exe[MAX_PATH];

    (void)argc; (void)argv;

    g_ssh = RegisterServiceCtrlHandlerA(SERVICE_NAME, ctrl_handler);
    if (!g_ssh) return;

    g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_stop) {
        report_status(SERVICE_STOPPED, 0);
        return;
    }

    report_status(SERVICE_START_PENDING, START_WAIT_MS);

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    default_conf_path(exe, conf, sizeof(conf));
    log_msg("service: startup " APPBOOT_VERSION
            ", configuration file: %s", conf);

    report_status(SERVICE_RUNNING, 0);

    /* supervisor mode: keep the model + handles and serve the control
     * interface so user processes can start/stop/restart/status apps */
    if (sup_load(conf) == 0) {
        HANDLE th;
        sup_boot();
        th = CreateThread(NULL, 0, serve_thr, NULL, 0, NULL);
        WaitForSingleObject(g_stop, INFINITE);
        /* release the accept loop: a self client sends QUIT */
        {
            ipc_conn *c = ipc_client_open(g_ep);
            char      r[64];
            if (c) {
                ipc_client_call(c, "QUIT", r, sizeof(r));
                ipc_client_close(c);
            }
            g_supquit = 1;
            if (th) {
                WaitForSingleObject(th, 3000);
                CloseHandle(th);
            }
        }
    } else {
        log_msg("service: configuration load failed; no apps started");
    }

    /* Stay running until the SCM asks us to stop; the launched
     * applications are intentionally left running. */
    report_status(SERVICE_STOPPED, 0);
}

/* ------------------------------------------------------------------ */
/* install / uninstall / start / stop helpers                           */
/* ------------------------------------------------------------------ */

int svc_control(const char *what, int needs_create)
{
    SC_HANDLE scm = NULL, svc = NULL;
    int ok = 0;

    scm = OpenSCManagerA(NULL, NULL,
                         needs_create ? SC_MANAGER_ALL_ACCESS
                                      : SERVICE_QUERY_STATUS | SERVICE_START |
                                        SERVICE_STOP | SERVICE_QUERY_CONFIG |
                                        DELETE);
    if (!scm) {
        log_msg("%s: OpenSCManager failed err=%lu", what,
                (unsigned long)GetLastError());
        return 1;
    }

    if (needs_create) {
        char exe[MAX_PATH];
        char binpath[MAX_PATH + 32];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        /* the SCM starts us with the "service" argument at every boot */
        sprintf(binpath, "\"%s\" service", exe);
        svc = CreateServiceA(scm, SERVICE_NAME, SERVICE_NAME,
                             SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START,        /* start at boot */
                             SERVICE_ERROR_NORMAL,
                             binpath, NULL, NULL, NULL, NULL, NULL);
        if (svc) {
            log_msg("%s: service '%s' created (auto start)", what, SERVICE_NAME);
            ok = 1;
            CloseServiceHandle(svc);
        }
    } else {
        svc = OpenServiceA(scm, SERVICE_NAME,
                           strcmp(what, "uninstall") == 0 ? DELETE
                                                         : SERVICE_START | SERVICE_STOP);
        if (svc) {
            if (strcmp(what, "uninstall") == 0) {
                if (DeleteService(svc)) {
                    log_msg("%s: service '%s' removed", what, SERVICE_NAME);
                    ok = 1;
                }
            } else if (strcmp(what, "start") == 0) {
                if (StartServiceA(svc, 0, NULL)) {
                    log_msg("%s: service started", what);
                    ok = 1;
                }
            } else { /* stop */
                SERVICE_STATUS st;
                if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
                    log_msg("%s: service stopped", what);
                    ok = 1;
                }
            }
            CloseServiceHandle(svc);
        }
    }

    if (!ok)
        log_msg("%s: failed err=%lu", what, (unsigned long)GetLastError());
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [command] [-c config-file] [-s control-endpoint]\n"
        "  default config: <exe dir>\\..\\conf\\startup.conf\n"
        "  (no command)  open the configuration/status GUI window\n"
        "  service       run as Windows service (SCM entry point)\n"
        "  serve         foreground supervisor with the control IPC\n"
        "  install       create service '%s' with automatic start\n",
        prog, SERVICE_NAME);
    fprintf(stderr,
        "  uninstall     remove the service\n"
        "  start | stop  control the installed service\n"
        "  run | console run the launcher in this console (debug)\n"
        "  -s NAME       control pipe name for 'app' commands "
        "(env: BOOTLAUNCHER_CTL)\n");
    cli_usage(prog);
}

#define MAX_POS 32

int main(int argc, char **argv)
{
    char    *pos[MAX_POS];            /* positional words (command+args) */
    int      npos = 0;
    const char *conf_arg = NULL;
    const char *ep_arg = NULL;
    char  conf[MAX_PATH * 2];
    char  exe[MAX_PATH];
    int   i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            conf_arg = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            ep_arg = argv[++i];
        } else if (npos < MAX_POS) {
            pos[npos++] = argv[i];
        }
    }

    if (ep_arg) {
        strncpy(g_ep, ep_arg, sizeof(g_ep) - 1);
        g_ep[sizeof(g_ep) - 1] = '\0';
        cli_set_endpoint(g_ep);
    }

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    default_conf_path(exe, conf, sizeof(conf));
    if (conf_arg) {
        strncpy(conf, conf_arg, sizeof(conf) - 1);
        conf[sizeof(conf) - 1] = '\0';
    }

    if (npos == 0) {
        /* no arguments: interactive configuration/status window */
        FreeConsole();
        return gui_main(conf);
    }

    if (strcmp(pos[0], "service") == 0) {
        SERVICE_TABLE_ENTRYA tbl[2];
        tbl[0].lpServiceName = (LPSTR)SERVICE_NAME;
        tbl[0].lpServiceProc = service_main;
        tbl[1].lpServiceName = NULL;
        tbl[1].lpServiceProc = NULL;

        if (!StartServiceCtrlDispatcherA(tbl)) {
            DWORD e = GetLastError();
            if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
                log_msg("not started by the SCM; use '%s console' to debug",
                        argv[0] ? argv[0] : "startup");
            else
                log_msg("StartServiceCtrlDispatcher failed err=%lu",
                        (unsigned long)e);
            return 2;
        }
        return 0;
    }

    if (strcmp(pos[0], "serve") == 0) {
        /* foreground supervisor: same control interface as the service,
         * no SCM registration (for desktop use and testing) */
        if (sup_load(conf) != 0) return 1;
        sup_boot();
        serve_loop();
        return 0;
    }

    if (strcmp(pos[0], "install") == 0)   return svc_control("install", 1);
    if (strcmp(pos[0], "uninstall") == 0) return svc_control("uninstall", 0);
    if (strcmp(pos[0], "start") == 0)     return svc_control("start", 0);
    if (strcmp(pos[0], "stop") == 0)      return svc_control("stop", 0);
    if (strcmp(pos[0], "console") == 0)   pos[0] = (char *)"run";
    if (strcmp(pos[0], "-V") == 0 || strcmp(pos[0], "--version") == 0)
        pos[0] = (char *)"version";

    if (cli_is_known(pos[0]))
        return cli_main(npos, pos, argv[0] ? argv[0] : "startup",
                        conf, run_all);

    usage(argv[0] ? argv[0] : "startup");
    return 2;
}
