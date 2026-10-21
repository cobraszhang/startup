/* ipc_posix.c - AF_UNIX stream-socket transport for the control channel.
 * Server: the supervisor process. Client: CLI user processes.
 * Portable C89 + POSIX.1-2001.
 */
#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"

/* never leak control-connection fds into launched applications:
 * an inherited fd would keep the reply socket from reaching EOF */
static void set_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

struct ipc_conn { int fd; };

/* ---------------- client ---------------- */

ipc_conn *ipc_client_open(const char *endpoint)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ipc_conn *c;

    if (fd < 0) return NULL;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(endpoint) >= sizeof(sa.sun_path)) {
        close(fd);
        return NULL;
    }
    strcpy(sa.sun_path, endpoint);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return NULL;                    /* nobody listening */
    }
    set_cloexec(fd);
    c = (ipc_conn *)malloc(sizeof(*c));
    if (!c) { close(fd); return NULL; }
    c->fd = fd;
    return c;
}

int ipc_client_write(ipc_conn *c, const char *data, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(c->fd, data + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return (int)done;
}

int ipc_client_read(ipc_conn *c, char *buf, size_t n)
{
    ssize_t r = read(c->fd, buf, n);
    if (r < 0) {
        if (errno == EINTR) return 0;   /* treat as retry-EOF, benign */
        return -1;
    }
    return (int)r;                      /* 0 = EOF */
}

void ipc_client_close(ipc_conn *c)
{
    if (!c) return;
    close(c->fd);
    free(c);
}

/* ---------------- server ---------------- */

struct ipc_srv { int lfd; int cfd; };

void *ipc_server_open(const char *endpoint)
{
    struct sockaddr_un sa;
    struct ipc_srv *srv;
    int on = 1;

    if (strlen(endpoint) >= sizeof(sa.sun_path)) return NULL;

    srv = (struct ipc_srv *)malloc(sizeof(*srv));
    if (!srv) return NULL;
    srv->lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    srv->cfd = -1;
    if (srv->lfd < 0) { free(srv); return NULL; }

    setsockopt(srv->lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    unlink(endpoint);                   /* clear stale socket file */

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, endpoint);
    if (bind(srv->lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(srv->lfd, 8) != 0) {
        close(srv->lfd);
        free(srv);
        return NULL;
    }
    set_cloexec(srv->lfd);
    return (void *)srv;
}

int ipc_server_accept(void *v)
{
    struct ipc_srv *srv = (struct ipc_srv *)v;
    int fd = accept(srv->lfd, NULL, NULL);
    if (fd < 0) return -1;              /* EINTR: interrupted by signal */
    set_cloexec(fd);
    srv->cfd = fd;
    return 0;
}

int ipc_server_readline(void *v, char *buf, size_t sz)
{
    struct ipc_srv *srv = (struct ipc_srv *)v;
    size_t got = 0;

    while (got + 1 < sz) {
        char ch;
        ssize_t r = read(srv->cfd, &ch, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return got ? (int)got : -1;
        if (ch == '\n') break;
        buf[got++] = ch;
    }
    buf[got] = '\0';
    return (int)got;
}

int ipc_server_write(void *v, const char *data)
{
    struct ipc_srv *srv = (struct ipc_srv *)v;
    size_t n = strlen(data), done = 0;

    while (done < n) {
        ssize_t w = write(srv->cfd, data + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return (int)done;
}

void ipc_server_closeconn(void *v)
{
    struct ipc_srv *srv = (struct ipc_srv *)v;
    if (srv->cfd >= 0) { close(srv->cfd); srv->cfd = -1; }
}

void ipc_server_destroy(void *v)
{
    struct ipc_srv *srv = (struct ipc_srv *)v;
    if (!srv) return;
    ipc_server_closeconn(v);
    close(srv->lfd);
    free(srv);
}
