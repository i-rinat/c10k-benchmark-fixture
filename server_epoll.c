#include "http_parser.h"
#include "macro.h"
#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 100
#define BUF_SIZE (16 * 1024)

#if 0
#define TRACE(...) printf(__VA_ARGS__)
#else
#define TRACE(...) do {} while (0)
#endif


static int g_epoll_fd;

static const char *const g_addr = "0.0.0.0";
static const int g_port = 65001;
static const char *const g_backend_addr = "127.0.0.1";
static const int g_backend_port = 8080;

static int g_server_socket;

struct buf {
    char *start;
    char *end;
    char *pos;
    char *last;
};

struct connection {
    int fd;
    struct buf buf;
    struct hp_state *parser;
    GString *s;
    int to_write;
};

struct the_event {
    int fd;
    int listening;
    int upstream;
    struct connection *cn;
    void (*read_handler)(struct the_event *);
    void (*write_handler)(struct the_event *);
    int read_ready;
    int write_ready;
    struct the_event *client_event;
    struct the_event *upstream_event;
    int finalizing;
};

static void
setnonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        err(1, "setnonblocking, fcntl, F_GETFL");

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        err(1, "setnonblocking, fcntl, F_SETFL");
}

static struct connection *
new_connection(void)
{
    struct connection *cn = calloc(sizeof(*cn), 1);
    if (!cn)
        errx(1, "malloc");

    cn->buf.start = malloc(BUF_SIZE);
    if (!cn->buf.start)
        errx(1, "malloc");

    cn->buf.end = cn->buf.start + BUF_SIZE;
    cn->buf.pos = cn->buf.start;
    cn->buf.last = cn->buf.start;

    cn->parser = hp_new();

    cn->s = g_string_new(NULL);

    return cn;
}

static void
close_connections(struct the_event *e)
{
    TRACE("%s\n", __func__);
    if (e->client_event)
        e = e->client_event;

    int ret = epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, e->fd, NULL);
    if (ret != 0)
        warn("close_connections: epoll_ctl");

    close(e->fd);

    if (e->upstream_event) {
        ret = epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, e->upstream_event->fd, NULL);
        if (ret != 0)
            warn("close_connections: epoll_ctl");

        close(e->upstream_event->fd);
    }

    free(e->cn->buf.start);
    g_string_free(e->cn->s, TRUE);
    free(e->cn);
    free(e->upstream_event);
    free(e->client_event);
    free(e);
}

static void
pipe_send_buf(struct the_event *e)
{
    TRACE("%s\n", __func__);
    assert(!e->upstream);

    struct buf *buf = &e->cn->buf;


    while (1) {
        if (buf->pos == buf->last) {
            if (e->finalizing)
                return close_connections(e);

            buf->pos = buf->start;
            buf->last = buf->start;

            goto try_other_side_of_a_pipe;
        }

        int data_written =
            RETRY_ON_EINTR(write(e->fd, buf->pos, buf->last - buf->pos));

        if (data_written == -1) {
            if (errno == EAGAIN) {
                size_t sz = buf->last - buf->pos;

                assert(sz != 0);
                warnx("%s: memmove used", __func__);
                memmove(buf->start, buf->pos, sz);
                buf->pos = buf->start;
                buf->last = buf->pos + sz;

                e->write_ready = 0;
                return;
            }
        }

        if (data_written == 0) {
            // Client connection was closed.
            warn("%s: written 0 bytes", __func__);
            return close_connections(e);
        }

        buf->pos += data_written;
    }

try_other_side_of_a_pipe:
    if (e->upstream_event->read_ready && e->upstream_event->read_handler) {
        return e->upstream_event->read_handler(e->upstream_event);
    }
}

static void
pipe_recv_buf(struct the_event *e)
{
    TRACE("%s\n", __func__);
    assert(e->upstream);

    struct buf *buf = &e->cn->buf;

    if (buf->last == buf->end)
        goto try_other_side_of_a_pipe;

    int data_read =
        RETRY_ON_EINTR(read(e->fd, buf->last, buf->end - buf->last));
    TRACE("  data_read=%d\n", data_read);

    if (data_read == -1) {
        if (errno == EAGAIN) {
            e->read_ready = 0;
            return;
        }
    }

    if (data_read == 0) {
        // The upstream closed the connection, nothing more to read.
        e->client_event->finalizing = 1;
        e->read_handler = NULL;
        goto try_other_side_of_a_pipe;
    }

    buf->last += data_read;

try_other_side_of_a_pipe:
    if (e->client_event->write_ready && e->client_event->write_handler) {
        return e->client_event->write_handler(e->client_event);
    }

    return;
}

static void
send_upstream_request(struct the_event *e)
{
    TRACE("%s\n", __func__);
    assert(e->upstream);

    if (e->cn->to_write == 0)
        goto next;

    while (1) {
        GString *s = e->cn->s;
        int written = RETRY_ON_EINTR(
            write(e->fd, s->str + (s->len - e->cn->to_write), e->cn->to_write));

        if (written == -1) {
            if (errno == EAGAIN) {
                e->write_ready = 0;
                return;
            }

            warn("%s: can't write", __func__);
            return;
        }

        if (written == 0) {
            return close_connections(e);
        }

        e->cn->to_write -= written;

        if (e->cn->to_write == 0)
            goto next;
    }

    return;

next:
    e->write_handler = NULL;
    e->read_handler = pipe_recv_buf;
    if (e->read_ready)
        e->read_handler(e);
}

static void
connect_to_upstream(struct the_event *e)
{
    TRACE("%s\n", __func__);
    assert(!e->upstream);

    struct sockaddr_in upstream = {};
    upstream.sin_family = AF_INET;
    upstream.sin_addr.s_addr = inet_addr(g_backend_addr);
    upstream.sin_port = htons(g_backend_port);

    int upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream_fd == -1) {
        warnx("socket, upstream");
        goto err;
    }

    int ret = RETRY_ON_EINTR(
        connect(upstream_fd, (struct sockaddr *)&upstream, sizeof(upstream)));
    if (ret == -1 && errno != EINPROGRESS) {
        perror("connect, upstream");
        goto err;
    }

    setnonblocking(upstream_fd);

    struct the_event *upstream_event = calloc(sizeof(*upstream_event), 1);
    upstream_event->fd = upstream_fd;
    upstream_event->upstream = 1;
    upstream_event->client_event = e;
    upstream_event->cn = e->cn;
    e->upstream_event = upstream_event;
    upstream_event->write_handler = send_upstream_request;

    struct epoll_event ee = {.events = EPOLLIN | EPOLLOUT | EPOLLET,
                             .data.ptr = upstream_event};
    ret = epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, upstream_fd, &ee);
    if (ret == -1)
        warn("epoll_ctl, EPOLL_CTL_ADD");

    e->cn->buf.pos = e->cn->buf.start;
    e->cn->buf.last = e->cn->buf.start;
    e->write_handler = pipe_send_buf;

    e->cn->to_write = e->cn->s->len;

    return;

err:
    close_connections(e);
}

static void
read_client_request(struct the_event *e)
{
    TRACE("%s\n", __func__);
    assert(!e->upstream);

    struct connection *cn = e->cn;

    while (1) {
        char buf[16 * 1024];
        int data_read = RETRY_ON_EINTR(read(e->fd, buf, sizeof(buf)));
        if (data_read == -1) {
            e->read_ready = 0;
            if (errno == EAGAIN)
                return;

            warn("%s: read", __func__);
            return;
        }

        if (data_read == 0) {
            warnx("%s: connection closed", __func__);
            return close_connections(e);
        }

        hp_parse_chunk(cn->parser, buf, data_read);
        if (cn->parser->header_parsed) {
            g_string_append_len(cn->s, buf,
                                data_read - cn->parser->remainder_sz);
            e->read_handler = NULL;
            e->write_handler = connect_to_upstream;
            if (e->write_ready)
                e->write_handler(e);
            return;
        }

        g_string_append_len(cn->s, buf, data_read);
    }
}

static void
handle_accept(int server_fd)
{
    TRACE("%s\n", __func__);

    int fd = accept(server_fd, NULL, NULL);
    if (fd == -1) {
        warn("accept");
        return;
    }

    setnonblocking(fd);

    struct the_event *e = calloc(sizeof(*e), 1);
    e->fd = fd;
    e->read_handler = read_client_request;
    e->cn = new_connection();

    struct epoll_event ee = {.events = EPOLLIN | EPOLLOUT | EPOLLET,
                             .data.ptr = e};
    int ret = epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ee);
    if (ret == -1)
        warn("epoll_ctl, EPOLL_CTL_ADD");
}

int
main(void)
{
    g_epoll_fd = epoll_create(1);
    if (g_epoll_fd == -1)
        err(1, "epoll_create");

    g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_socket == -1)
        err(1, "socket");

    int reuseaddr = 1;
    int ret = setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
                         sizeof(int));
    if (ret == -1)
        err(1, "setsockopt SO_REUSEADDR");

    struct sockaddr_in addr = {};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(g_addr);
    addr.sin_port = htons(g_port);

    ret = bind(g_server_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1)
        err(1, "bind");

    ret = listen(g_server_socket, SOMAXCONN);
    if (ret == -1)
        err(1, "listen");

    setnonblocking(g_server_socket);

    struct the_event *e = calloc(sizeof(*e), 1);
    e->fd = g_server_socket;
    e->listening = 1;

    struct epoll_event ee = {.events = EPOLLIN, .data.ptr = e};

    ret = epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_server_socket, &ee);

    while (1) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(g_epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1)
            err(1, "epoll_wait");

        for (int k = 0; k < nfds; k++) {
            struct the_event *e = events[k].data.ptr;
            if (e->listening) {
                handle_accept(e->fd);
                continue;
            }

            if (events[k].events & EPOLLIN) {
                e->read_ready = 1;
                if (e->read_handler)
                    e->read_handler(e);
            }

            if (events[k].events & EPOLLOUT) {
                e->write_ready = 1;
                if (e->write_handler)
                    e->write_handler(e);
            }
        }
    }

    return 0;
}
