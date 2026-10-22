/* ipc_win32.c - named-pipe transport for the control channel.
 * Server: the service process. Client: CLI / GUI user processes.
 * VC6 / MinGW, plain C89 + Win32 API.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ipc.h"
#include "../src/log.h"

struct ipc_conn { HANDLE h; };

/* ---------------- client ---------------- */

ipc_conn *ipc_client_open(const char *endpoint)
{
    ipc_conn *c;
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        HANDLE h = CreateFileA(endpoint, GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            c = (ipc_conn *)malloc(sizeof(*c));
            if (!c) { CloseHandle(h); return NULL; }
            c->h = h;
            return c;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) {
            /* another client is being served; wait and retry */
            if (!WaitNamedPipeA(endpoint, 2000)) break;
            continue;
        }
        return NULL;    /* no such pipe: service not running */
    }
    return NULL;
}

int ipc_client_write(ipc_conn *c, const char *data, size_t n)
{
    DWORD wr = 0;
    if (!WriteFile(c->h, data, (DWORD)n, &wr, NULL)) return -1;
    return (int)wr;
}

int ipc_client_read(ipc_conn *c, char *buf, size_t n)
{
    DWORD rd = 0;
    if (!ReadFile(c->h, buf, (DWORD)n, &rd, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return 0; /* server closed */
        return -1;
    }
    return (int)rd;
}

void ipc_client_close(ipc_conn *c)
{
    if (!c) return;
    CloseHandle(c->h);
    free(c);
}

/* ---------------- server ---------------- */

/* A service runs as LocalSystem; the default pipe DACL from its token
 * does NOT grant access to interactive users, so user-session clients
 * get "access denied". Build an explicit DACL: creator, administrators
 * and the system keep full control, interactive users get read+write. */
#ifndef SDDL_REVISION_1
#define SDDL_REVISION_1 1
#endif
#ifdef __GNUC__
#include <sddl.h>          /* MinGW declares the SDDL helper here */
#else
/* VC6 SDK headers do not declare it */
BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorA(
    LPCSTR, DWORD, PSECURITY_DESCRIPTOR *, PDWORD);
#endif

static PSECURITY_DESCRIPTOR server_sd(void)
{
    static PSECURITY_DESCRIPTOR sd = NULL;
    if (!sd) {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:P(A;;GA;;;CO)(A;;GA;;;BA)(A;;GA;;;SY)(A;;GRGW;;;IU)",
                SDDL_REVISION_1, &sd, NULL)) {
            log_msg("ipc: pipe security descriptor conversion failed err=%lu",
                    (unsigned long)GetLastError());
            sd = (PSECURITY_DESCRIPTOR)-1;   /* remember the failure */
        }
    }
    return sd == (PSECURITY_DESCRIPTOR)-1 ? NULL : sd;
}

void *ipc_server_open(const char *endpoint)
{
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = server_sd();
    HANDLE h;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    /* NOTE: the last CreateNamedPipe argument is a SECURITY_ATTRIBUTES,
     * not a bare security descriptor: passing the descriptor directly
     * compiles (void*) but is silently ignored at runtime. */
    h = CreateNamedPipeA(endpoint,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                      /* one instance; clients serialise */
        4096, 4096,
        0, sd ? &sa : NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return (void *)h;
}

int ipc_server_accept(void *srv)
{
    HANDLE h = (HANDLE)srv;
    if (ConnectNamedPipe(h, NULL)) return 0;
    if (GetLastError() == ERROR_PIPE_CONNECTED) return 0; /* raced client */
    return -1;
}

int ipc_server_readline(void *srv, char *buf, size_t sz)
{
    HANDLE h = (HANDLE)srv;
    size_t got = 0;

    while (got + 1 < sz) {
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, 1, &rd, NULL) || rd == 0)
            return got ? (int)got : -1;
        if (buf[got] == '\n') break;
        got++;
    }
    buf[got < sz ? got : sz - 1] = '\0';
    return (int)got;
}

int ipc_server_write(void *srv, const char *data)
{
    HANDLE h = (HANDLE)srv;
    DWORD wr = 0, n = (DWORD)strlen(data);
    if (!WriteFile(h, data, n, &wr, NULL)) return -1;
    FlushFileBuffers(h);
    return (int)wr;
}

void ipc_server_closeconn(void *srv)
{
    DisconnectNamedPipe((HANDLE)srv);
}

void ipc_server_destroy(void *srv)
{
    CloseHandle((HANDLE)srv);
}
