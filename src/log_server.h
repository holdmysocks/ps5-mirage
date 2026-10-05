/*
 * Mirage - the payload's log, kept in memory and readable over the network.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * When the title starts the payload, the loader socket that carries stdout
 * is closed at once, so the log would be lost. The last LOG_SERVER_BYTES of
 * it are kept here, and anyone who connects to TCP port LOG_SERVER_PORT gets
 * a copy and is disconnected:
 *     curl.exe -s telnet://<console-ip>:47011     (or any raw TCP client)
 * Read-only, nothing is ever read from the connection. It is on the LAN, and
 * the log names the devices that connect; it is a development aid.
 */
#ifndef MIRAGE_LOG_SERVER_H
#define MIRAGE_LOG_SERVER_H

#include <stddef.h>

#define LOG_SERVER_PORT 47011
#define LOG_SERVER_BYTES (256 * 1024)

/* Adds text to the kept log. Safe from any thread, before or after start. */
void log_server_append(const char *text, size_t length);
/* Starts serving. Returns 0, or -1 if the port could not be opened (the log
 * is still kept, only not served). */
int log_server_start(void);
void log_server_stop(void);

#endif
