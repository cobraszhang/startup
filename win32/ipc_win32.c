/* ipc_win32.c - named-pipe transport for the control channel.
 * Server: the service process. Client: CLI / GUI user processes.
 * VC6 / MinGW, plain C89 + Win32 API.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"

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

void *ipc_server_open(const char *endpoint)
{
    HANDLE h = CreateNamedPipeA(endpoint,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                      /* one instance; clients serialise */
        4096, 4096,
        0, NULL);
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
