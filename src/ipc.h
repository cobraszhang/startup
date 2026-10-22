/* ipc.h - control channel between the service process and user processes.
 *
 * The service process (started at boot) keeps a runtime table of the
 * sub-application processes it launched and serves a line-based control
 * interface on a platform IPC endpoint:
 *
 *   Win32  : named pipes  \\.\pipe\AppBootLauncher       (system side)
 *                          \\.\pipe\AppBootLauncherUser  (user side)
 *   POSIX  : AF_UNIX sockets /tmp/bootlauncher.ctl       (system side)
 *                           /tmp/bootlauncher-user.ctl   (user side)
 *
 * The endpoint can be overridden with the BOOTLAUNCHER_CTL environment
 * variable or the -s command line option (client and service alike).
 *
 * Protocol: one request per connection. The client sends a single text
 * line, the service replies with one or more lines and closes.
 *   PING | LIST | STATUS [name] | START name | STOP name
 *   RESTART name | QUIT
 * Replies start with "OK" or "ERR <code>"; the CLI 'app' subcommands
 * and the service share this grammar through ipc_parse()/ipc_dispatch().
 *
 * Transport functions are implemented per platform (ipc_win32.c,
 * ipc_posix.c); the protocol logic is shared (ipc.c). Portable C89.
 */
#ifndef IPC_H
#define IPC_H

#include <stddef.h>

#define IPC_ENDPOINT_ENV    "BOOTLAUNCHER_CTL"
#ifdef _WIN32
#  define IPC_DEFAULT_ENDPOINT "\\\\.\\pipe\\AppBootLauncher"
#  define IPC_USER_ENDPOINT    "\\\\.\\pipe\\AppBootLauncherUser"
#else
#  define IPC_DEFAULT_ENDPOINT "/tmp/bootlauncher.ctl"
#  define IPC_USER_ENDPOINT    "/tmp/bootlauncher-user.ctl"
#endif

#define IPC_LINE_MAX 512

/* request operations parsed from the first token (case-insensitive) */
enum ipc_op {
    IPC_OP_PING = 0, IPC_OP_LIST, IPC_OP_STATUS, IPC_OP_START,
    IPC_OP_STOP, IPC_OP_RESTART, IPC_OP_QUIT, IPC_OP_BAD
};

/* Parse one request line: fills op and optional name argument. */
void ipc_parse(const char *line, int *op, char *name, size_t namesz);

/* Run one request through the platform-provided ops callback.
 * The callback must write a complete reply ("OK ..." / "ERR ...")
 * into out. ipc_dispatch handles IPC_OP_BAD itself.
 * Returns the op executed (so the server can detect IPC_OP_QUIT). */
int ipc_dispatch(const char *line, char *out, size_t outsz,
                 int (*ops)(int, const char *, char *, size_t));

/* ---------------- client (used by the CLI in user processes) --------- */

typedef struct ipc_conn ipc_conn;   /* opaque, defined per platform */

/* endpoint from IPC_ENDPOINT_ENV if set, else the platform default for
 * the given startup class (0 = system side, 1 = user side) */
void ipc_default_endpoint(char *out, size_t outsz, int kind);

ipc_conn *ipc_client_open(const char *endpoint);   /* NULL when absent */
/* byte primitives implemented per platform (0 EOF, -1 error) */
int ipc_client_write(ipc_conn *c, const char *data, size_t n);
int ipc_client_read(ipc_conn *c, char *buf, size_t n);
/* send one request line, read the whole reply until close; returns
 * reply length, or -1 on transport error (implemented in ipc.c) */
int       ipc_client_call(ipc_conn *c, const char *request,
                          char *reply, size_t replysz);
void      ipc_client_close(ipc_conn *c);

/* ---------------- server (used by the service process) --------------- */

void *ipc_server_open(const char *endpoint);    /* NULL on failure */
int   ipc_server_accept(void *srv);             /* 0 ok, -1 error/stop */
int   ipc_server_readline(void *srv, char *buf, size_t sz);
int   ipc_server_write(void *srv, const char *data);
void  ipc_server_closeconn(void *srv);          /* end this connection */
void  ipc_server_destroy(void *srv);            /* remove the endpoint */

#endif /* IPC_H */
