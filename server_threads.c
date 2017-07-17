#include "http_parser.h"
#include "macro.h"
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static const char *g_addr = "0.0.0.0";
static const int g_port = 65001;
static const char *g_backend_addr = "127.0.0.1";
static const int g_backend_port = 8080;
static const int g_thread_count = 10000;

static int g_server_socket;
static pthread_t *g_threads;
static volatile gint g_connection_count = 0;

static void
connection_count_report(int diff)
{
#if 0
    int current_count = g_atomic_int_add(&g_connection_count, diff) + diff;
    printf("connection count = %d\n", current_count);
#endif
}

static void *
connection_handler(void *param)
{
    while (1) {
        int fd = RETRY_ON_EINTR(accept(g_server_socket, NULL, NULL));
        if (fd == -1) {
            perror("accept");
            goto err_1;
        }

        struct timeval timeout = {.tv_sec = 60};

        int ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (ret == -1) {
            perror("setsockopt SO_RCVTIMEO");
            goto err_2;
        }

        ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (ret == -1) {
            perror("setsockopt SO_SNDTIMEO");
            goto err_2;
        }

        connection_count_report(1);

        struct hp_state *state = hp_new();
        if (!state) {
            warnx("hp_new");
            goto err_2;
        }

        GString *s = g_string_new(NULL);

        char buf[16 * 1024];
        while (1) {
            int data_read = RETRY_ON_EINTR(read(fd, buf, sizeof(buf)));
            if (data_read == 0)
                goto err_2;

            if (data_read == -1) {
                perror("read");
                goto err_2;
            }

            hp_parse_chunk(state, buf, data_read);
            if (state->header_parsed) {
                g_string_append_len(s, buf, data_read - state->remainder_sz);
                break;
            }

            g_string_append_len(s, buf, data_read);
        }

        struct sockaddr_in upstream = {};
        upstream.sin_family = AF_INET;
        upstream.sin_addr.s_addr = inet_addr(g_backend_addr);
        upstream.sin_port = htons(g_backend_port);

        int upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (upstream_fd == -1) {
            warnx("socket, upstream");
            goto err_2;
        }

        ret = RETRY_ON_EINTR(connect(upstream_fd, (struct sockaddr *)&upstream, sizeof(upstream)));
        if (ret == -1) {
            perror("connect, upstream");
            goto err_3;
        }

        // Send request to the upstream.
        int to_write = s->len;
        while (to_write > 0) {
            int written = RETRY_ON_EINTR(write(upstream_fd, buf + (s->len - to_write), to_write));
            if (written < 0) {
                perror("write, upstream");
                goto err_3;
            }
            to_write -= written;
        }

        g_string_free(s, TRUE);

        // Pump.
        while (1) {
            int data_read = RETRY_ON_EINTR(read(upstream_fd, buf, sizeof(buf)));
            if (data_read == 0)
                break;

            if (data_read < 0) {
                perror("read");
                goto err_3;
            }

            int to_write = data_read;
            while (to_write > 0) {
                int written = RETRY_ON_EINTR(write(fd, buf + (data_read - to_write), to_write));
                if (written < 0) {
                    perror("write, client");
                    goto err_3;
                }
                to_write -= written;
            }
        }

    err_3:
        close(upstream_fd);
    err_2:
        close(fd);
    err_1:
        connection_count_report(-1);
        continue;
    }

    return NULL;
}

int
main(void)
{
    g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_socket == -1)
        err(1, "socket");

    int reuseaddr = 1;
    int ret = setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int));
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

    g_threads = malloc(g_thread_count * sizeof(pthread_t));
    if (!g_threads)
        errx(1, "malloc");

    for (int k = 0; k < g_thread_count; k++) {
        int ret = pthread_create(&g_threads[k], NULL, connection_handler, NULL);
        if (ret == -1)
            err(1, "pthread_create");
    }

    printf("serving\n");

    for (int k = 0; k < g_thread_count; k++)
        pthread_join(g_threads[k], NULL);

    return 0;
}
