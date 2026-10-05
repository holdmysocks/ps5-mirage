/*
 * Mirage - numeric address parsing for UxPlay on the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Payload SDK's getaddrinfo answers EAI_FAMILY to every AF_INET6 request.
 * UxPlay parses the client's address with getaddrinfo(AI_NUMERICHOST), so a
 * client that connected over IPv6 made raop_ntp_init return NULL, and the
 * library then dereferenced it. UxPlay's lib is compiled with
 * -Dgetaddrinfo=mirage_getaddrinfo -Dfreeaddrinfo=mirage_freeaddrinfo.
 *
 * Only what UxPlay asks for is implemented: a numeric IPv4 or IPv6 host (the
 * latter with an optional %zone, numeric or an interface name), no service.
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int mirage_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result);
void mirage_freeaddrinfo(struct addrinfo *info);

static struct addrinfo *new_info(int family, const void *address, socklen_t length,
                                 const struct addrinfo *hints)
{
    struct addrinfo *info = calloc(1, sizeof(*info) + length);
    if (!info)
        return NULL;
    info->ai_family = family;
    info->ai_socktype = hints ? hints->ai_socktype : 0;
    info->ai_protocol = hints ? hints->ai_protocol : 0;
    info->ai_addrlen = length;
    info->ai_addr = (struct sockaddr *)(info + 1);
    memcpy(info->ai_addr, address, length);
    return info;
}

int mirage_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result)
{
    const int family = hints ? hints->ai_family : AF_UNSPEC;

    if (!result)
        return EAI_FAIL;
    *result = NULL;
    if (!node)
        return EAI_NONAME;
    if (service)
        return EAI_SERVICE;

    if (family == AF_INET || family == AF_UNSPEC) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_len = sizeof(sin);
        sin.sin_family = AF_INET;
        if (inet_pton(AF_INET, node, &sin.sin_addr) == 1) {
            *result = new_info(AF_INET, &sin, sizeof(sin), hints);
            return *result ? 0 : EAI_MEMORY;
        }
        if (family == AF_INET)
            return EAI_NONAME;
    }

    if (family == AF_INET6 || family == AF_UNSPEC) {
        struct sockaddr_in6 sin6;
        char host[64];
        const char *zone = strchr(node, '%');
        size_t host_length = zone ? (size_t)(zone - node) : strlen(node);

        if (host_length >= sizeof(host))
            return EAI_NONAME;
        memcpy(host, node, host_length);
        host[host_length] = '\0';

        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_len = sizeof(sin6);
        sin6.sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, host, &sin6.sin6_addr) != 1)
            return EAI_NONAME;
        if (zone && zone[1]) {
            char *end = NULL;
            unsigned long index = strtoul(zone + 1, &end, 10);
            if (!end || *end != '\0')
                index = if_nametoindex(zone + 1);
            sin6.sin6_scope_id = (uint32_t)index;
        }
        *result = new_info(AF_INET6, &sin6, sizeof(sin6), hints);
        return *result ? 0 : EAI_MEMORY;
    }

    return EAI_FAMILY;
}

void mirage_freeaddrinfo(struct addrinfo *info)
{
    while (info) {
        struct addrinfo *next = info->ai_next;
        free(info);
        info = next;
    }
}
