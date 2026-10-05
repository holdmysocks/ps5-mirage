/*
 * Mirage - payload side of the PCM link: a loopback listener for the title.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pcm_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "pcm_link.h"

#define SEND_TIMEOUT_MS 200
#define POLL_US 200000

static pcm_server_log_t say;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int listener = -1;
static int client = -1; /* guarded by lock */
static atomic_int running;
static atomic_int ever_connected;
static atomic_long last_seen; /* CLOCK_MONOTONIC seconds a title was last connected, or the start */
static atomic_ulong dropped;
/* What the title has sent so far of a message, and the name from its hello.
 * Guarded by lock. */
static uint8_t inbox[PCM_LINK_HEADER_SIZE + PCM_LINK_HELLO_MAX];
static size_t inbox_used;
static char hello_name[PCM_LINK_HELLO_MAX + 1];
static int have_hello;
static atomic_int quit_requested;
static char advertised_name[PCM_LINK_NAME_MAX + 1]; /* guarded by lock */
static char shown_pin[PCM_LINK_PIN_MAX + 1];         /* guarded by lock */

/* Mirrored video that arrived while no title was connected. A mirroring
 * stream has one keyframe, at its start, and every later picture builds on
 * the ones before; a title that the payload has just asked the system to
 * open connects seconds into the stream and could never show it. So the
 * units since the last keyframe are kept and handed to the title first, and
 * its decoder runs through them to catch up. Guarded by lock. */
#define BACKLOG_MAX_BYTES (16u * 1024 * 1024)
typedef struct backlog_unit {
    struct backlog_unit *next;
    int64_t time_ns;
    uint32_t length;
    uint8_t h265, nal_count;
    uint8_t data[];
} backlog_unit_t;
static backlog_unit_t *backlog_first, *backlog_last;
static uint32_t backlog_bytes;
static unsigned backlog_units;
static int backlog_usable; /* it starts at a keyframe and nothing since was lost */
static uint8_t last_size_body[PCM_LINK_VIDEO_SIZE_BODY];
static int have_last_size;

static void backlog_clear_locked(void)
{
    while (backlog_first) {
        backlog_unit_t *next = backlog_first->next;
        free(backlog_first);
        backlog_first = next;
    }
    backlog_last = NULL;
    backlog_bytes = 0;
    backlog_units = 0;
    backlog_usable = 0;
}

/* Whether an Annex-B unit holds a picture that needs no earlier one. */
static int is_keyframe(int h265, const uint8_t *data, uint32_t length)
{
    for (uint32_t i = 0; i + 4 < length; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        if (h265) {
            const unsigned type = (data[i + 3] >> 1) & 0x3f;
            if (type >= 16 && type <= 21)
                return 1;
        } else if ((data[i + 3] & 0x1f) == 5) {
            return 1;
        }
    }
    return 0;
}

static void backlog_add_locked(int64_t time_ns, int h265, int nal_count, const uint8_t *unit, uint32_t length)
{
    backlog_unit_t *kept;

    if (is_keyframe(h265, unit, length)) {
        backlog_clear_locked();
        backlog_usable = 1;
    }
    if (!backlog_usable)
        return; /* nothing to build on: wait for the next keyframe */
    kept = backlog_bytes + length <= BACKLOG_MAX_BYTES ? malloc(sizeof(*kept) + length) : NULL;
    if (!kept) { /* too much, or no memory: without this unit the rest is useless */
        backlog_clear_locked();
        return;
    }
    kept->next = NULL;
    kept->time_ns = time_ns;
    kept->length = length;
    kept->h265 = (uint8_t)(h265 != 0);
    kept->nal_count = (uint8_t)(nal_count > 255 ? 255 : (nal_count < 0 ? 0 : nal_count));
    memcpy(kept->data, unit, length);
    if (backlog_last)
        backlog_last->next = kept;
    else
        backlog_first = kept;
    backlog_last = kept;
    backlog_bytes += length;
    ++backlog_units;
}

static long monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static void drop_client_locked(const char *why)
{
    if (client >= 0) {
        close(client);
        client = -1;
        atomic_store(&last_seen, monotonic_seconds());
        say("link: title disconnected (%s)", why);
    }
}

static int send_all(int fd, const void *data, size_t length)
{
    const uint8_t *p = data;
    while (length > 0) {
        ssize_t n = send(fd, p, length, 0);
        if (n <= 0)
            return -1; /* includes the send timeout: the title stopped reading */
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

/* Reads what the title has sent, without waiting. The only message it sends
 * is a hello; an end of the connection means the title is gone. */
static void check_client_locked(void)
{
    ssize_t n;

    if (client < 0)
        return;
    n = recv(client, inbox + inbox_used, sizeof(inbox) - inbox_used, MSG_DONTWAIT);
    if (n == 0) {
        drop_client_locked("closed by the title");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            drop_client_locked("connection error");
        return;
    }
    inbox_used += (size_t)n;
    if (inbox_used >= PCM_LINK_HEADER_SIZE) {
        const uint32_t length = (uint32_t)inbox[4] | (uint32_t)inbox[5] << 8 | (uint32_t)inbox[6] << 16 |
                                (uint32_t)inbox[7] << 24;
        if (inbox[0] == 'M' && inbox[1] == 'P' && inbox[2] == PCM_LINK_QUIT && length == 0) {
            atomic_store(&quit_requested, 1);
            drop_client_locked("it was a newer copy of the payload, asking this one to stop");
            return;
        }
        if (inbox[0] != 'M' || inbox[1] != 'P' || inbox[2] != PCM_LINK_HELLO || length > PCM_LINK_HELLO_MAX) {
            drop_client_locked("sent something that is not a hello");
            return;
        }
        if (inbox_used >= PCM_LINK_HEADER_SIZE + length) {
            memcpy(hello_name, inbox + PCM_LINK_HEADER_SIZE, length);
            hello_name[length] = '\0';
            have_hello = 1;
            inbox_used = 0; /* nothing else is expected on this connection */
        }
    }
}

static int send_message_locked(uint8_t type, uint8_t flags, const void *first, uint32_t first_length,
                               const void *second, uint32_t second_length);

/* Hands the kept video to the title that has just connected. The title takes
 * it as fast as its decoder goes, so this may wait far longer than a normal
 * send is allowed to. */
static void flush_backlog_locked(void)
{
    struct timeval patient = {5, 0}, normal = {0, SEND_TIMEOUT_MS * 1000};
    const unsigned units = backlog_units;
    const uint32_t bytes = backlog_bytes;
    int ok = 1;

    if (client < 0 || !backlog_first)
        return;
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &patient, sizeof(patient));
    if (have_last_size)
        ok = send_message_locked(PCM_LINK_VIDEO_SIZE, 0, last_size_body, sizeof(last_size_body), NULL, 0) == 0;
    for (backlog_unit_t *unit = backlog_first; ok && unit; unit = unit->next) {
        uint8_t prefix[PCM_LINK_VIDEO_PREFIX] = {0};
        pcm_link_put_time(prefix, unit->time_ns);
        prefix[8] = unit->h265 ? PCM_LINK_CODEC_H265 : PCM_LINK_CODEC_H264;
        prefix[9] = unit->nal_count;
        ok = send_message_locked(PCM_LINK_VIDEO, 0, prefix, sizeof(prefix), unit->data, unit->length) == 0;
    }
    backlog_clear_locked();
    if (!ok) {
        drop_client_locked("it did not take the video kept for it");
        return;
    }
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &normal, sizeof(normal));
    say("link: handed the title %u video units (%u bytes) from before it connected", units, (unsigned)bytes);
}

static void *accept_main(void *arg)
{
    (void)arg;
    while (atomic_load(&running)) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        struct timeval timeout = {0, SEND_TIMEOUT_MS * 1000};
        int one = 1;
        /* The listener is non-blocking so that stopping never waits on accept. */
        int fd = accept(listener, (struct sockaddr *)&from, &fromlen);
        if (fd < 0) {
            pthread_mutex_lock(&lock);
            check_client_locked();
            if (client >= 0)
                atomic_store(&last_seen, monotonic_seconds());
            pthread_mutex_unlock(&lock);
            usleep(POLL_US);
            continue;
        }
        /* An accepted socket can inherit the listener's non-blocking flag. */
        fcntl(fd, F_SETFL, 0);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_mutex_lock(&lock);
        drop_client_locked("replaced by a new connection");
        client = fd;
        inbox_used = 0;
        if (advertised_name[0] != '\0' &&
            send_message_locked(PCM_LINK_NAME, 0, advertised_name, (uint32_t)strlen(advertised_name), NULL, 0) != 0)
            drop_client_locked("it did not take the name");
        if (client >= 0 && shown_pin[0] != '\0' &&
            send_message_locked(PCM_LINK_PIN, 0, shown_pin, (uint32_t)strlen(shown_pin), NULL, 0) != 0)
            drop_client_locked("it did not take the pairing code");
        flush_backlog_locked();
        atomic_store(&last_seen, monotonic_seconds());
        pthread_mutex_unlock(&lock);
        atomic_store(&ever_connected, 1);
        say("link: title connected");
    }
    return NULL;
}

/* Tells the copy of the payload that holds the link port to stop. */
static void ask_running_copy_to_quit(const struct sockaddr_in *addr)
{
    uint8_t header[PCM_LINK_HEADER_SIZE];
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return;
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0) {
        pcm_link_header(header, PCM_LINK_QUIT, 0, 0);
        (void)send_all(fd, header, sizeof(header));
        usleep(300000); /* let it read the message before the socket goes */
    }
    close(fd);
}

int pcm_server_start(pcm_server_log_t log)
{
    struct sockaddr_in addr;
    int one = 1;
    int bound = 0;

    say = log;
    atomic_store(&last_seen, monotonic_seconds());
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PCM_LINK_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* Up to about 10 s: once straight, then after asking the other copy to go. */
    for (int attempt = 0; attempt < 40 && !bound; ++attempt) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) {
            say("link: socket errno %d", errno);
            return -1;
        }
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(listener, 2) == 0) {
            bound = 1;
            break;
        }
        close(listener);
        listener = -1;
        if (errno != EADDRINUSE) {
            say("link: bind/listen 127.0.0.1:%d errno %d", PCM_LINK_PORT, errno);
            return -1;
        }
        if (attempt == 0) {
            say("link: port %d is taken: asking the running copy to stop", PCM_LINK_PORT);
            ask_running_copy_to_quit(&addr);
        }
        usleep(250000);
    }
    if (!bound) {
        say("link: port %d is still taken after asking", PCM_LINK_PORT);
        return -1;
    }
    if (fcntl(listener, F_SETFL, O_NONBLOCK) != 0) {
        say("link: non-blocking errno %d", errno);
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
    say("link: listening on 127.0.0.1:%d", PCM_LINK_PORT);
    return 0;
}

void pcm_server_stop(void)
{
    if (!atomic_exchange(&running, 0))
        return;
    pthread_join(thread, NULL);
    pthread_mutex_lock(&lock);
    drop_client_locked("server stopping");
    pthread_mutex_unlock(&lock);
    close(listener);
    listener = -1;
}

/* One message to the connected title. Returns 0 if it went out whole. */
static int send_message_locked(uint8_t type, uint8_t flags, const void *first, uint32_t first_length,
                               const void *second, uint32_t second_length)
{
    uint8_t header[PCM_LINK_HEADER_SIZE];

    pcm_link_header(header, type, flags, first_length + second_length);
    if (send_all(client, header, sizeof(header)) != 0 ||
        (first_length > 0 && send_all(client, first, first_length) != 0) ||
        (second_length > 0 && send_all(client, second, second_length) != 0))
        return -1;
    return 0;
}

static void send_parts(uint8_t type, uint8_t flags, const void *first, uint32_t first_length, const void *second,
                       uint32_t second_length)
{
    if (first_length + second_length > PCM_LINK_MAX_BODY)
        return;
    pthread_mutex_lock(&lock);
    if (client < 0) {
        atomic_fetch_add(&dropped, 1);
    } else if (send_message_locked(type, flags, first, first_length, second, second_length) != 0) {
        drop_client_locked("send failed or timed out");
        atomic_fetch_add(&dropped, 1);
    }
    pthread_mutex_unlock(&lock);
}

void pcm_server_send(uint8_t type)
{
    if (type == PCM_LINK_VIDEO_RESET) { /* that stream is over: nothing of it is worth keeping */
        pthread_mutex_lock(&lock);
        backlog_clear_locked();
        pthread_mutex_unlock(&lock);
    }
    send_parts(type, 0, NULL, 0, NULL, 0);
}

void pcm_server_send_hold(int hold)
{
    const uint8_t body = hold ? 1 : 0;
    send_parts(PCM_LINK_HOLD, 0, &body, 1, NULL, 0);
}

void pcm_server_send_audio(int64_t time_ns, int live, const int16_t *samples, uint32_t sample_bytes)
{
    uint8_t prefix[PCM_LINK_AUDIO_PREFIX];
    pcm_link_put_time(prefix, time_ns);
    send_parts(PCM_LINK_AUDIO, live ? PCM_LINK_FLAG_LIVE : 0, prefix, sizeof(prefix), samples, sample_bytes);
}

void pcm_server_send_video(int64_t time_ns, int h265, int nal_count, const uint8_t *unit, uint32_t unit_bytes)
{
    uint8_t prefix[PCM_LINK_VIDEO_PREFIX] = {0};
    pcm_link_put_time(prefix, time_ns);
    prefix[8] = h265 ? PCM_LINK_CODEC_H265 : PCM_LINK_CODEC_H264;
    prefix[9] = (uint8_t)(nal_count > 255 ? 255 : (nal_count < 0 ? 0 : nal_count));
    if (unit_bytes > PCM_LINK_MAX_BODY - PCM_LINK_VIDEO_PREFIX)
        return;
    /* Sent, or kept for the title that will come: one or the other, decided
     * under the lock, so that a title connecting at this moment gets the
     * unit exactly once. */
    pthread_mutex_lock(&lock);
    if (client < 0) {
        backlog_add_locked(time_ns, h265, nal_count, unit, unit_bytes);
        atomic_fetch_add(&dropped, 1);
    } else if (send_message_locked(PCM_LINK_VIDEO, 0, prefix, sizeof(prefix), unit, unit_bytes) != 0) {
        drop_client_locked("send failed or timed out");
        atomic_fetch_add(&dropped, 1);
    }
    pthread_mutex_unlock(&lock);
}

int pcm_server_send_film_video(int64_t time_ns, const uint8_t *unit, uint32_t unit_bytes)
{
    uint8_t prefix[PCM_LINK_VIDEO_PREFIX] = {0};
    int sent = -1;

    if (unit_bytes > PCM_LINK_MAX_BODY - PCM_LINK_VIDEO_PREFIX)
        return -1;
    pcm_link_put_time(prefix, time_ns);
    prefix[8] = PCM_LINK_CODEC_H264;
    pthread_mutex_lock(&lock);
    if (client >= 0) {
        /* After a seek the title has seconds of pictures to run through and
         * takes units only as fast as its decoder goes: be patient here. */
        struct timeval patient = {5, 0}, normal = {0, SEND_TIMEOUT_MS * 1000};
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &patient, sizeof(patient));
        if (send_message_locked(PCM_LINK_VIDEO, PCM_LINK_FLAG_ORDERED, prefix, sizeof(prefix), unit, unit_bytes) == 0) {
            sent = 0;
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &normal, sizeof(normal));
        } else {
            drop_client_locked("the film's video was not taken in 5 s");
        }
    }
    pthread_mutex_unlock(&lock);
    return sent;
}

void pcm_server_send_video_size(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height)
{
    const uint32_t values[4] = {source_width, source_height, width, height};
    uint8_t body[PCM_LINK_VIDEO_SIZE_BODY];
    for (int i = 0; i < 4; ++i)
        for (int b = 0; b < 4; ++b)
            body[4 * i + b] = (uint8_t)(values[i] >> (8 * b));
    pthread_mutex_lock(&lock);
    memcpy(last_size_body, body, sizeof(body));
    have_last_size = 1;
    pthread_mutex_unlock(&lock);
    send_parts(PCM_LINK_VIDEO_SIZE, 0, body, sizeof(body), NULL, 0);
}

int pcm_server_hello(char *name, size_t size)
{
    int have;
    pthread_mutex_lock(&lock);
    have = have_hello;
    if (have && size > 0) {
        strncpy(name, hello_name, size - 1);
        name[size - 1] = '\0';
    }
    pthread_mutex_unlock(&lock);
    return have;
}

void pcm_server_set_name(const char *name)
{
    pthread_mutex_lock(&lock);
    snprintf(advertised_name, sizeof(advertised_name), "%s", name ? name : "");
    if (client >= 0 && advertised_name[0] != '\0' &&
        send_message_locked(PCM_LINK_NAME, 0, advertised_name, (uint32_t)strlen(advertised_name), NULL, 0) != 0)
        drop_client_locked("it did not take the name");
    pthread_mutex_unlock(&lock);
}

void pcm_server_set_pin(const char *pin)
{
    pthread_mutex_lock(&lock);
    snprintf(shown_pin, sizeof(shown_pin), "%s", pin ? pin : "");
    if (client >= 0 && send_message_locked(PCM_LINK_PIN, 0, shown_pin, (uint32_t)strlen(shown_pin), NULL, 0) != 0)
        drop_client_locked("it did not take the pairing code");
    pthread_mutex_unlock(&lock);
}

int pcm_server_quit_requested(void)
{
    return atomic_load(&quit_requested);
}

int pcm_server_connected(void)
{
    int connected;
    pthread_mutex_lock(&lock);
    connected = client >= 0;
    pthread_mutex_unlock(&lock);
    return connected;
}

int pcm_server_ever_connected(void)
{
    return atomic_load(&ever_connected);
}

unsigned pcm_server_seconds_without_title(void)
{
    long gone;
    if (pcm_server_connected())
        return 0;
    gone = monotonic_seconds() - atomic_load(&last_seen);
    return gone > 0 ? (unsigned)gone : 0;
}

unsigned long pcm_server_dropped(void)
{
    return atomic_load(&dropped);
}
