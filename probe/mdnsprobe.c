/*
 * Mirage - mDNS reachability probe for the PS5 (open question 1).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs as a payload through the ELF loader, so stdout is the loader's TCP
 * socket and every line reaches the PC before the step it describes runs.
 *
 * STAGE selects how far the probe goes, one new operation per run:
 *   1  bind UDP 0.0.0.0:5353 (SO_REUSEADDR/SO_REUSEPORT), wait for a unicast
 *      datagram from the PC
 *   2  stage 1 + join 224.0.0.251 and listen for multicast traffic
 *   3  stage 2 + send one standard query for _airplay._tcp.local
 *
 * The probe only blocks in usleep; it never spins, changes no priority or
 * privilege, touches no file and exits by itself.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef STAGE
#define STAGE 1
#endif

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define LISTEN_SECONDS 12
#define POLL_US 20000
#define MAX_PACKETS_LOGGED 25

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The first question name of a DNS message, dotted, for the log. */
static void first_name(const unsigned char *p, int len, char *out, int outlen)
{
    int pos = 12, o = 0;
    out[0] = '\0';
    if (len < 13 || (p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0))
        return;
    while (pos < len && p[pos] != 0 && p[pos] < 64) {
        int n = p[pos++];
        if (pos + n > len || o + n + 2 > outlen)
            break;
        memcpy(out + o, p + pos, (size_t)n);
        o += n;
        out[o++] = '.';
        pos += n;
    }
    out[o] = '\0';
    for (int i = 0; i < o; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e)
            out[i] = '?';
}

static void listen_for(int fd, int seconds)
{
    const double deadline = now_s() + seconds;
    /* Second limit, independent of the clock and of usleep doing its job. */
    long budget = (long)seconds * 1000000L / POLL_US + 50;
    int packets = 0, multicast_named = 0;

    say("listen: up to %d s on fd %d", seconds, fd);
    while (now_s() < deadline && budget-- > 0) {
        unsigned char buf[1500];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                say("listen: recvfrom errno %d (%s), stopping", errno, strerror(errno));
                break;
            }
            usleep(POLL_US);
            continue;
        }
        ++packets;
        if (packets <= MAX_PACKETS_LOGGED) {
            char name[128];
            char src[32];
            first_name(buf, (int)n, name, sizeof(name));
            if (name[0])
                ++multicast_named;
            inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
            say("  rx %4zd bytes from %s:%u %s q=%u an=%u %s", n, src,
                (unsigned)ntohs(from.sin_port),
                n >= 3 && (buf[2] & 0x80) ? "response" : "query",
                n >= 6 ? (unsigned)((buf[4] << 8) | buf[5]) : 0,
                n >= 8 ? (unsigned)((buf[6] << 8) | buf[7]) : 0, name);
        }
    }
    say("listen: done, %d datagram(s), %d with a question name", packets, multicast_named);
}

int main(void)
{
    struct sockaddr_in addr;
    int fd, one = 1, rc;

    say("mdnsprobe: stage %d, pid %d", STAGE, (int)getpid());

    /* Which local address the stack would use towards the group (UxPlay's
     * mdnsd finds its address this way). Uses only connect + getsockname. */
    say("step: local address towards %s", MDNS_GROUP);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct sockaddr_in local;
        socklen_t locallen = sizeof(local);
        char text[32] = "?";
        memset(&addr, 0, sizeof(addr));
        addr.sin_len = sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, MDNS_GROUP, &addr.sin_addr);
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (rc != 0)
            say("  connect errno %d (%s)", errno, strerror(errno));
        else if (getsockname(fd, (struct sockaddr *)&local, &locallen) != 0)
            say("  getsockname errno %d (%s)", errno, strerror(errno));
        else
            say("  local address %s", inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)));
        close(fd);
    } else {
        say("  socket errno %d (%s)", errno, strerror(errno));
    }

    say("step: socket(AF_INET, SOCK_DGRAM)");
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        say("RESULT: socket failed, errno %d (%s)", errno, strerror(errno));
        return 1;
    }

    say("step: SO_REUSEADDR");
    rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    say("  rc %d errno %d", rc, rc ? errno : 0);
    say("step: SO_REUSEPORT");
    rc = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    say("  rc %d errno %d", rc, rc ? errno : 0);

    say("step: bind 0.0.0.0:%d", MDNS_PORT);
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        say("RESULT: bind failed, errno %d (%s)", errno, strerror(errno));
        close(fd);
        return 1;
    }
    say("  bound");

#if STAGE >= 2
    {
        struct ip_mreq mreq;
        unsigned char ttl = 255, loop = 0;

        say("step: IP_ADD_MEMBERSHIP %s on INADDR_ANY", MDNS_GROUP);
        memset(&mreq, 0, sizeof(mreq));
        inet_pton(AF_INET, MDNS_GROUP, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            say("RESULT: join failed, errno %d (%s)", errno, strerror(errno));
            close(fd);
            return 1;
        }
        say("  joined");
        say("step: IP_MULTICAST_TTL 255, IP_MULTICAST_LOOP 0");
        rc = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        say("  ttl rc %d errno %d", rc, rc ? errno : 0);
        rc = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        say("  loop rc %d errno %d", rc, rc ? errno : 0);
    }
#endif

#if STAGE >= 3
    {
        /* Standard one-question query: PTR _airplay._tcp.local, QU bit set. */
        static const unsigned char query[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x08, '_', 'a', 'i', 'r', 'p', 'l', 'a', 'y',
            0x04, '_', 't', 'c', 'p',
            0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
            0x00, 0x0c, 0x80, 0x01};
        ssize_t sent;

        say("step: send a %zu byte query to %s:%d", sizeof(query), MDNS_GROUP, MDNS_PORT);
        memset(&addr, 0, sizeof(addr));
        addr.sin_len = sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, MDNS_GROUP, &addr.sin_addr);
        sent = sendto(fd, query, sizeof(query), 0, (struct sockaddr *)&addr, sizeof(addr));
        if (sent < 0)
            say("  sendto errno %d (%s)", errno, strerror(errno));
        else
            say("  sent %zd bytes", sent);
    }
#endif

    listen_for(fd, LISTEN_SECONDS);

    say("step: close");
    close(fd);
    say("RESULT: stage %d finished", STAGE);
    return 0;
}
