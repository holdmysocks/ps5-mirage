/*
 * Mirage - opening the title from the payload.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MIRAGE_LAUNCHER_H
#define MIRAGE_LAUNCHER_H

typedef struct {
    int ran;     /* the helper answered */
    int user;    /* result of asking for the foreground user, 0 if there is one */
    int running; /* app id of the running title (> 0), or <= 0 for none */
    int launch;  /* > 0 the title was launched, < 0 refused, 0 not tried */
} launcher_result_t;

/* Sends the launcher helper (helper\launcher.c, carried inside this payload)
 * to the ELF loader on loopback and waits a few seconds for its one-line
 * answer. The helper launches the Mirage title only if no app is running.
 * Returns 0 if the helper answered, -1 otherwise. Blocks: call it from a
 * thread that may wait. */
int launcher_run(launcher_result_t *result);

/* A system notification (the pop-up at the top of the screen). */
void launcher_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
