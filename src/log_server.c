/*
 * Mirage - the payload's log, kept in memory and readable over the network.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "log_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char ring[LOG_SERVER_BYTES]; /* guarded by lock */
static size_t ring_start, ring_used;
static pthread_t thread;
static int listener = -1;
static atomic_int running;

void log_server_append(const char *text, size_t length)
{
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < length; ++i) {
        if (ring_used == LOG_SERVER_BYTES) { /* full: the oldest byte goes */
            ring_start = (ring_start + 1) % LOG_SERVER_BYTES;
            --ring_used;
        }
        ring[(ring_start + ring_used) % LOG_SERVER_BYTES] = text[i];
        ++ring_used;
    }
    pthread_mutex_unlock(&lock);
}

static void serve(int fd)
{
    struct timeval timeout = {2, 0};
    char *copy;
    size_t length, sent = 0;

    /* Copy first, send afterwards: a slow reader must not hold up logging. */
    pthread_mutex_lock(&lock);
    length = ring_used;
    copy = malloc(length ? length : 1);
    if (copy)
        for (size_t i = 0; i < length; ++i)
            copy[i] = ring[(ring_start + i) % LOG_SERVER_BYTES];
    pthread_mutex_unlock(&lock);

    fcntl(fd, F_SETFL, 0);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (copy && sent < length) {
        ssize_t n = send(fd, copy + sent, length - sent, 0);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
    free(copy);
    close(fd);
}

static void *accept_main(void *arg)
{
    (void)arg;
    while (atomic_load(&running)) {
        int fd = accept(listener, NULL, NULL); /* non-blocking */
        if (fd < 0) {
            usleep(250000);
            continue;
        }
        serve(fd);
    }
    return NULL;
}

int log_server_start(void)
{
    struct sockaddr_in addr;
    int one = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return -1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LOG_SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 2) != 0 ||
        fcntl(listener, F_SETFL, O_NONBLOCK) != 0) {
        close(listener);
        listener = -1;
        return -1;
    }
    atomic_store(&running, 1);
    if (pthread_create(&thread, NULL, accept_main, NULL) != 0) {
        atomic_store(&running, 0);
        close(listener);
        listener = -1;
        return -1;
    }
    return 0;
}

void log_server_stop(void)
{
    if (!atomic_exchange(&running, 0))
        return;
    pthread_join(thread, NULL);
    close(listener);
    listener = -1;
}
