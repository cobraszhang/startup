/* ipc.c - shared control-protocol logic (portable C89).
 * Request grammar and the generic client "call" helper; the byte
 * transports live in ipc_win32.c / ipc_posix.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ipc.h"

static void upcopy(char *dst, size_t dstsz, const char *src, size_t n)
{
    size_t i;
    if (n >= dstsz) n = dstsz - 1;
    for (i = 0; i < n; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[n] = '\0';
}

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

void ipc_parse(const char *line, int *op, char *name, size_t namesz)
{
    static const struct { const char *verb; int op; } tbl[] = {
        { "PING",    IPC_OP_PING    }, { "LIST",    IPC_OP_LIST  },
        { "STATUS",  IPC_OP_STATUS  }, { "START",   IPC_OP_START },
        { "STOP",    IPC_OP_STOP    }, { "RESTART", IPC_OP_RESTART },
        { "QUIT",    IPC_OP_QUIT    }
    };
    const char *p = line;
    char        verb[16];
    size_t      start;

    *op = IPC_OP_BAD;
    if (name && namesz) name[0] = '\0';

    while (*p == ' ' || *p == '\t') p++;
    start = (size_t)(p - line);
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    upcopy(verb, sizeof(verb), line + start, (size_t)(p - line - start));

    while (*p == ' ' || *p == '\t') p++;
    if (name && namesz) {
        size_t len = strlen(p);
        if (len >= namesz) len = namesz - 1;
        memcpy(name, p, len);
        name[len] = '\0';
        trim_end(name);
    }

    {
        size_t i;
        for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
            if (strcmp(verb, tbl[i].verb) == 0) { *op = tbl[i].op; return; }
        }
    }
}

int ipc_dispatch(const char *line, char *out, size_t outsz,
                 int (*ops)(int, const char *, char *, size_t))
{
    int  op;
    char name[256];

    ipc_parse(line, &op, name, sizeof(name));
    if (op == IPC_OP_BAD) {
        strncpy(out, "ERR bad request (PING|LIST|STATUS [name]|START|STOP|"
                     "RESTART name|QUIT)",
                outsz - 1);
        out[outsz - 1] = '\0';
        return op;
    }
    return ops(op, name, out, outsz);
}

/* ---------------- generic client over the byte primitives ----------- */

void ipc_default_endpoint(char *out, size_t outsz)
{
    const char *e = getenv(IPC_ENDPOINT_ENV);
    strncpy(out, (e && *e) ? e : IPC_DEFAULT_ENDPOINT, outsz - 1);
    out[outsz - 1] = '\0';
}

int ipc_client_call(ipc_conn *c, const char *request,
                    char *reply, size_t replysz)
{
    char    line[IPC_LINE_MAX];
    size_t  n;
    size_t  got = 0;
    int     r;

    n = strlen(request);
    if (n + 2 > sizeof(line)) return -1;
    memcpy(line, request, n);
    line[n] = '\n';
    line[n + 1] = '\0';
    if (ipc_client_write(c, line, n + 1) <= 0) return -1;

    for (;;) {
        if (got + 1 >= replysz) break;
        r = ipc_client_read(c, reply + got, replysz - 1 - got);
        if (r <= 0) break;                 /* EOF or error */
        got += (size_t)r;
    }
    reply[got] = '\0';
    if (got == 0) return -1;
    return (int)got;
}
