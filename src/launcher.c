/*
 * Mirage - opening the title from the payload.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "launcher.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef LAUNCHER_ELF
#error "LAUNCHER_ELF must name the built launcher helper (tools\build.ps1 passes it)"
#endif

#define LOADER_PORT 9021
#define ANSWER_WAIT_MS 8000
#define ANSWER_KEY "MIRAGE-LAUNCHER "

extern const uint8_t launcher_helper[];
extern const uint8_t launcher_helper_end[];

__asm__(".section .rodata\n"
        ".global launcher_helper\n"
        ".global launcher_helper_end\n"
        "launcher_helper:\n"
        ".incbin \"" LAUNCHER_ELF "\"\n"
        "launcher_helper_end:\n"
        ".text\n");

int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);

int launcher_run(launcher_result_t *result)
{
    const size_t size = (size_t)(launcher_helper_end - launcher_helper);
    struct sockaddr_in addr;
    char answer[1024];
    size_t sent = 0, got = 0;
    char *line;
    int fd;

    memset(result, 0, sizeof(*result));
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LOADER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    while (sent < size) {
        ssize_t n = send(fd, launcher_helper + sent, size - sent, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    /* The helper's output comes back on this socket. Poll, never block: the
     * system library prints lines of its own first, and the loader may or
     * may not close the socket when the helper ends. */
    for (int waited = 0; waited < ANSWER_WAIT_MS && got < sizeof(answer) - 1; waited += 50) {
        ssize_t n = recv(fd, answer + got, sizeof(answer) - 1 - got, MSG_DONTWAIT);
        if (n == 0)
            break;
        if (n > 0) {
            got += (size_t)n;
            answer[got] = '\0';
            line = strstr(answer, ANSWER_KEY);
            if (line && strchr(line, '\n'))
                break;
            continue;
        }
        usleep(50000);
    }
    close(fd);
    answer[got] = '\0';
    line = strstr(answer, ANSWER_KEY);
    if (!line || sscanf(line, ANSWER_KEY "user=%d running=%d launch=%d", &result->user, &result->running,
                        &result->launch) != 3)
        return -1;
    result->ran = 1;
    return 0;
}

void launcher_notify(const char *fmt, ...)
{
    /* The layout the SDK's samples and the references use: 45 bytes the
     * kernel fills or ignores, then the text. */
    struct {
        uint8_t reserved[45];
        char message[3075];
    } request;
    va_list ap;

    memset(&request, 0, sizeof(request));
    va_start(ap, fmt);
    vsnprintf(request.message, sizeof(request.message), fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}
