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
 *   startup.exe install          create the service (auto start; if not
 *   startup.exe uninstall        administrator, both re-run themselves
 *                                elevated through the UAC prompt)
 *   startup.exe start | stop     control the installed service
 *   startup.exe run | console    run the launcher interactively (debug)
 *   startup.exe help             management subcommands (list/order/add/...)
 *   -c <file>                    use an alternate configuration file
 *
 * Compiles with MSVC 6.0 (/TC) and MinGW-w64 gcc -std=c89. C89 only.
 */
#include <windows.h>
#include <shellapi.h>
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

/* ------------------------------------------------------------------ */
/* execution-time interpreter resolution (documented in win32_app.h)   */
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

int app_build_cmdline(const char *stored, char *out, size_t outsz)
{
    char        path[MAX_PATH * 2];
    char        assoc[MAX_PATH * 2];
    char        ext[16];
    const char *p = stored;
    char       *dot;
    size_t      i;

    /* only a lone quoted path is interpreted; everything else (plain
     * commands, paths with extra arguments) is used exactly as stored */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') { passthrough(stored, out, outsz); return 0; }
    p++;
    i = 0;
    while (*p && *p != '"' && i + 1 < sizeof(path)) path[i++] = *p++;
    if (*p != '"') { passthrough(stored, out, outsz); return 0; }
    path[i] = '\0';
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) { passthrough(stored, out, outsz); return 0; }

    dot = strrchr(path, '.');
    ext[0] = '\0';
    if (dot) lower_copy(ext, sizeof(ext), dot + 1);

    /* real executables launch directly */
    if (!ext[0] || strcmp(ext, "exe") == 0 || strcmp(ext, "com") == 0 ||
        strcmp(ext, "pif") == 0) {
        passthrough(stored, out, outsz);
        return 0;
    }

    if (strcmp(ext, "bat") == 0 || strcmp(ext, "cmd") == 0) {
        if (strlen(path) + 16 >= outsz) return -1;
        sprintf(out, "cmd.exe /c \"%s\"", path);
        return 0;
    }

    if (strcmp(ext, "ps1") == 0) {
        if (SearchPathA(NULL, "powershell.exe", ".exe",
                        (DWORD)sizeof(assoc) - 1, assoc, NULL) == 0)
            return -1;               /* no PowerShell on this system */
        if (strlen(path) + 80 >= outsz) return -1;
        sprintf(out, "cmd.exe /c powershell.exe -NoProfile "
                     "-ExecutionPolicy Bypass -File \"%s\"", path);
        return 0;
    }

    /* remaining script types: use the shell-registered interpreter,
     * if the system has one (py / js / vbs / wsf / pl / ...) */
    assoc[0] = '\0';
    if ((int)(INT_PTR)FindExecutableA(path, NULL, assoc) <= 32 || !assoc[0])
        return -1;
    if (strlen(assoc) + strlen(path) + 6 >= outsz) return -1;
    sprintf(out, "\"%s\" \"%s\"", assoc, path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* process spawn helper                                                  */
/* ------------------------------------------------------------------ */

/* A LocalSystem service lives in session 0; a GUI application it
 * starts with plain CreateProcessA runs invisibly (session 0
 * isolation). When the caller holds SeTcbPrivilege (services do), the
 * application is started inside the ACTIVE CONSOLE session through
 * that user's token, so windows appear on the logged-in desktop.
 * Before anybody logs in (early boot) the token lookup fails and we
 * fall back to the caller's own session - console programs work
 * either way. In a user session the lookup fails for lack of
 * privilege and behaviour is unchanged. The WTS entry points are
 * resolved dynamically: the VC6 SDK predates their headers and older
 * systems may lack wtsapi32.dll. */
typedef DWORD (WINAPI *wts_active_session_fn)(void);
typedef BOOL (WINAPI *wts_query_user_token_fn)(DWORD, HANDLE *);
typedef BOOL (WINAPI *create_environment_block_fn)(LPVOID *, HANDLE, BOOL);
typedef BOOL (WINAPI *destroy_environment_block_fn)(LPVOID);

static int spawn_app(const char *cmdline, PROCESS_INFORMATION *pi)
{
    wts_active_session_fn     pActive = (wts_active_session_fn)0;
    wts_query_user_token_fn   pToken  = (wts_query_user_token_fn)0;
    create_environment_block_fn  pEnvBlk = (create_environment_block_fn)0;
    destroy_environment_block_fn pEnvFree = (destroy_environment_block_fn)0;
    static HMODULE            hWts    = NULL;
    static HMODULE            hUe     = NULL;
    LPVOID                    env     = NULL;
    HMODULE                   hK;
    DWORD                     session = 0xFFFFFFFF;
    HANDLE                    tok  = NULL, ptok = NULL;
    STARTUPINFOA              si;
    BOOL                      ok = FALSE;
    DWORD                     flags = CREATE_NO_WINDOW |
                                      CREATE_NEW_PROCESS_GROUP;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    /* transfer through a union: keeps strict C89 compilers from
     * warning about function-pointer casts of GetProcAddress */
    {
        union { FARPROC raw; wts_active_session_fn fn; } c1;
        hK = GetModuleHandleA("kernel32.dll");
        if (hK) {
            c1.raw = GetProcAddress(hK, "WTSGetActiveConsoleSessionId");
            pActive = c1.fn;
        }
    }
    if (!hWts) hWts = LoadLibraryA("wtsapi32.dll");
    if (hWts) {
        union { FARPROC raw; wts_query_user_token_fn fn; } c2;
        c2.raw = GetProcAddress(hWts, "WTSQueryUserToken");
        pToken = c2.fn;
    }

    if (!hUe) hUe = LoadLibraryA("userenv.dll");
    if (hUe) {
        union { FARPROC raw; create_environment_block_fn fn; } c3;
        union { FARPROC raw; destroy_environment_block_fn fn; } c4;
        c3.raw = GetProcAddress(hUe, "CreateEnvironmentBlock");
        c4.raw = GetProcAddress(hUe, "DestroyEnvironmentBlock");
        pEnvBlk  = c3.fn;
        pEnvFree = c4.fn;
    }

    if (pActive) session = pActive();
    if (pToken && session != 0xFFFFFFFF && pToken(session, &tok) &&
        DuplicateTokenEx(tok, TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE |
                         TOKEN_QUERY, NULL, SecurityImpersonation,
                         TokenPrimary, &ptok)) {
        /* the environment must be the one belonging to the user token:
         * inheriting the service's LocalSystem block would put
         * USERPROFILE/APPDATA under the systemprofile directory, which
         * the user account cannot write (apps died at first startup) */
        if (pEnvBlk && !pEnvBlk(&env, ptok, FALSE))
            env = NULL; /* fall back to inherited environment */
        ok = CreateProcessAsUserA(ptok, NULL, (LPSTR)cmdline,
                                  NULL, NULL, FALSE, flags,
                                  env, NULL, &si, pi);
    }
    if (env && pEnvFree) pEnvFree(env);
    if (tok)  CloseHandle(tok);
    if (ptok) CloseHandle(ptok);

    if (!ok)
        ok = CreateProcessA(NULL, (LPSTR)cmdline, NULL, NULL, FALSE,
                            flags, NULL, NULL, &si, pi);
    return ok ? 0 : (int)GetLastError();
}

static int launch_app(const app_t *a)
{
    PROCESS_INFORMATION pi;
    char cmdline[CMD_MAX];
    int  rc;

    if (app_build_cmdline(a->cmd, cmdline, sizeof(cmdline)) != 0) {
        log_msg("launch: no interpreter available for '%s' (cmd='%s')",
                a->name, a->cmd);
        return -1;
    }

    memset(&pi, 0, sizeof(pi));
    rc = spawn_app(cmdline, &pi);
    if (rc != 0) {
        log_msg("launch: FAILED '%s' (cmd='%s') err=%d",
                a->name, a->cmd, rc);
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
    PROCESS_INFORMATION pi;
    char cmdline[CMD_MAX];
    int  rc;

    if (app_build_cmdline(g_sapps[i].cmd, cmdline, sizeof(cmdline)) != 0) {
        log_msg("sup: no interpreter available for '%s' (cmd='%s')",
                g_sapps[i].name, g_sapps[i].cmd);
        return -1;
    }

    memset(&pi, 0, sizeof(pi));
    rc = spawn_app(cmdline, &pi);
    if (rc != 0) {
        log_msg("sup: start '%s' failed (cmd='%s') err=%d",
                g_sapps[i].name, g_sapps[i].cmd, rc);
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

static void setup_service_logging(const char *exe)
{
    char  dir[MAX_PATH * 2];
    char *slash;
    static char envbuf[MAX_PATH * 2 + 24];

    if (getenv("BOOTLAUNCHER_LOG")) return;      /* explicit wins */
    strncpy(dir, exe, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '\\');
    if (!slash) return;
    *slash = '\0';
    if (strlen(dir) + 20 >= sizeof(dir)) return;
    strcat(dir, "\\..\\logs");
    CreateDirectoryA(dir, NULL);
    strcat(dir, "\\startup.log");
    /* putenv (not SetEnvironmentVariableA): the C runtime getenv used
     * by log.c only sees the CRT's own environment block */
    strcpy(envbuf, "BOOTLAUNCHER_LOG=");
    strcat(envbuf, dir);
    putenv(envbuf);
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
    /* no console in session 0: keep the supervisor log in a file
     * next to the configuration (dist\logs\startup.log) unless the
     * BOOTLAUNCHER_LOG environment variable selects another path */
    setup_service_logging(exe);
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

/* Vista+ "Automatic (Delayed Start)": the SCM finishes ALL normal
 * auto-start services first, then starts this one - as part of the boot
 * flow, before any interactive user logs in. Manual 'start' is
 * unaffected. VC6 headers do not carry this info level. */
#ifndef SERVICE_CONFIG_DELAYED_AUTO_START_INFO
#define SERVICE_CONFIG_DELAYED_AUTO_START_INFO 3
#endif
typedef struct { BOOL fDelayedAutostart; } delayed_autostart_t;

static void set_delayed_autostart(SC_HANDLE svc)
{
    delayed_autostart_t  want;
    BYTE                 buf[64];
    DWORD                need = 0;

    want.fDelayedAutostart = TRUE;
    if (!ChangeServiceConfig2A(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                               &want)) {
        log_msg("install: could not enable delayed auto-start err=%lu",
                (unsigned long)GetLastError());
        return;
    }
    if (QueryServiceConfig2A(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                             buf, sizeof(buf), &need) &&
        need >= sizeof(delayed_autostart_t) &&
        ((delayed_autostart_t *)buf)->fDelayedAutostart)
        log_msg("install: boot start deferred until all auto-start "
                "services are up (delayed auto-start)");
    else
        log_msg("install: WARNING: delayed auto-start not confirmed");
}

/* Run this same executable once more with administrator rights
 * (ShellExecute "runas" shows the UAC prompt). Waits for the elevated
 * child and returns its exit code, or a negative Win32 error when the
 * launch itself failed (1223 = the user declined the prompt). */
static int run_elevated(const char *args)
{
    SHELLEXECUTEINFOA se;
    char exe[MAX_PATH];

    GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    memset(&se, 0, sizeof(se));
    se.cbSize       = sizeof(se);
    se.fMask        = SEE_MASK_NOCLOSEPROCESS;
    se.lpVerb       = "runas";
    se.lpFile       = exe;
    se.lpParameters = args;
    se.nShow        = SW_HIDE;
    if (!ShellExecuteExA(&se))
        return -(int)GetLastError();
    if (se.hProcess) {
        DWORD rc = 1;
        WaitForSingleObject(se.hProcess, 30000);
        GetExitCodeProcess(se.hProcess, &rc);
        CloseHandle(se.hProcess);
        return (int)rc;
    }
    return 0;
}

/* One-shot elevation retry for install/uninstall. The child carries a
 * marker environment variable so an already-elevated attempt that still
 * hits ACCESS_DENIED reports its error instead of looping. */
static int svc_elevated(const char *what)
{
    int rc;

    log_msg("%s: needs administrator rights; retrying elevated "
            "(please confirm the UAC prompt)...", what);
    /* putenv keeps the C runtime and the Win32 environment in sync:
     * the child needs the marker, and "NAME=" removes it afterwards */
    putenv((char *)"STARTUP_ELEV=1");
    rc = run_elevated(what);
    putenv((char *)"STARTUP_ELEV=");
    if (rc == 0) {
        log_msg("%s: completed elevated", what);
        return 0;
    }
    if (rc == -1223)
        log_msg("%s: elevation declined by the user", what);
    else
        log_msg("%s: elevated attempt failed (rc=%d)", what, rc);
    return 1;
}

int svc_control(const char *what, int needs_create)
{
    SC_HANDLE scm = NULL, svc = NULL;
    DWORD     err = 0;
    int ok = 0;
    /* manage commands may retry once through UAC */
    int manage  = (strcmp(what, "install") == 0 ||
                   strcmp(what, "uninstall") == 0);
    int already = getenv("STARTUP_ELEV") != NULL;

    scm = OpenSCManagerA(NULL, NULL,
                         needs_create ? SC_MANAGER_ALL_ACCESS
                                      : SC_MANAGER_CONNECT);
    if (!scm) {
        err = GetLastError();
        if (manage && !already && err == ERROR_ACCESS_DENIED)
            return svc_elevated(what);
        log_msg("%s: OpenSCManager failed err=%lu", what,
                (unsigned long)err);
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
            set_delayed_autostart(svc);
            log_msg("%s: service '%s' created (auto start, delayed)",
                    what, SERVICE_NAME);
            ok = 1;
            CloseServiceHandle(svc);
        } else {
            err = GetLastError();
        }
    } else {
        svc = OpenServiceA(scm, SERVICE_NAME,
                           strcmp(what, "uninstall") == 0
                               ? DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS
                                                         : SERVICE_START | SERVICE_STOP);
        if (svc) {
            if (strcmp(what, "uninstall") == 0) {
                /* DeleteService on a RUNNING service only marks it for
                 * deletion: it keeps running and stays listed until it
                 * exits. Stop it first (bounded wait), then delete. */
                SERVICE_STATUS st;
                int waited = 0;
                if (QueryServiceStatus(svc, &st) &&
                    st.dwCurrentState != SERVICE_STOPPED) {
                    ControlService(svc, SERVICE_CONTROL_STOP, &st);
                    while (waited < 10000 &&
                           QueryServiceStatus(svc, &st) &&
                           st.dwCurrentState != SERVICE_STOPPED) {
                        Sleep(200);
                        waited += 200;
                    }
                }
                if (DeleteService(svc)) {
                    log_msg("%s: service '%s' removed", what, SERVICE_NAME);
                    ok = 1;
                } else {
                    err = GetLastError();
                }
            } else if (strcmp(what, "start") == 0) {
                if (StartServiceA(svc, 0, NULL)) {
                    log_msg("%s: service started", what);
                    ok = 1;
                } else {
                    err = GetLastError();
                }
            } else { /* stop */
                SERVICE_STATUS st;
                if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
                    log_msg("%s: service stopped", what);
                    ok = 1;
                } else {
                    err = GetLastError();
                }
            }
            CloseServiceHandle(svc);
        } else {
            err = GetLastError();
        }
    }

    if (!ok && manage && !already && err == ERROR_ACCESS_DENIED) {
        CloseServiceHandle(scm);
        return svc_elevated(what);
    }

    if (!ok)
        log_msg("%s: failed err=%lu", what, (unsigned long)err);
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
