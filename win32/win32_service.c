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

#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "cli.h"
#include "win32_app.h"

#define CONF_SUBPATH   "..\\conf\\startup.conf"
#define CMD_MAX        4096
#define START_WAIT_MS  3000   /* wait hint while starting / stopping */

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
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
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
/* service entry                                                        */
/* ------------------------------------------------------------------ */

/* Default configuration: <exe dir>\..\conf\startup.conf */
void default_conf_path(const char *exepath, char *out, size_t outsz)
{
    char  *slash;
    size_t len;

    strncpy(out, exepath, outsz - 1);
    out[outsz - 1] = '\0';
    slash = strrchr(out, '\\');
    if (!slash) slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    len = strlen(out);
    if (len + 1 + strlen(CONF_SUBPATH) < outsz) {
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
    log_msg("service: configuration file: %s", conf);

    report_status(SERVICE_RUNNING, 0);
    run_all(conf);

    /* Stay running until the SCM asks us to stop or the system shuts
     * down; the launched applications keep running. */
    WaitForSingleObject(g_stop, INFINITE);
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
        "usage: %s [command] [-c config-file]\n"
        "  default config: <exe dir>\\..\\conf\\startup.conf\n"
        "  (no command)  open the configuration/status GUI window\n"
        "  service       run as Windows service (SCM entry point)\n"
        "  install       create service '%s' with automatic start\n"
        "  uninstall     remove the service\n"
        "  start | stop  control the installed service\n"
        "  run | console run the launcher in this console (debug)\n",
        prog, SERVICE_NAME);
    cli_usage(prog);
}

#define MAX_POS 32

int main(int argc, char **argv)
{
    char    *pos[MAX_POS];            /* positional words (command+args) */
    int      npos = 0;
    const char *conf_arg = NULL;
    char  conf[MAX_PATH * 2];
    char  exe[MAX_PATH];
    int   i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            conf_arg = argv[++i];
        } else if (npos < MAX_POS) {
            pos[npos++] = argv[i];
        }
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

    if (strcmp(pos[0], "install") == 0)   return svc_control("install", 1);
    if (strcmp(pos[0], "uninstall") == 0) return svc_control("uninstall", 0);
    if (strcmp(pos[0], "start") == 0)     return svc_control("start", 0);
    if (strcmp(pos[0], "stop") == 0)      return svc_control("stop", 0);
    if (strcmp(pos[0], "console") == 0)   pos[0] = (char *)"run";

    if (cli_is_known(pos[0]))
        return cli_main(npos, pos, argv[0] ? argv[0] : "startup",
                        conf, run_all);

    usage(argv[0] ? argv[0] : "startup");
    return 2;
}
