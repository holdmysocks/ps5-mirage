/*
 * Mirage - launcher helper: opens the Mirage title if the console is idle.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A tiny payload of its own. The Mirage payload carries it inside and sends
 * it to the ELF loader when a device connects and no title is there to play
 * what it sends. It asks which app is running; on the home screen it
 * launches the title, otherwise it launches nothing. It reports on its
 * standard output, which is the loader socket the Mirage payload is reading:
 *
 *     MIRAGE-LAUNCHER user=<rc> running=<n> launch=<n>
 *
 * running: the app id of the running title (> 0), or <= 0 for none.
 * launch:  the launch call's result (> 0 the new app id, < 0 refused), or 0
 *          if no launch was tried.
 *
 * It is separate so that the system service libraries are never loaded into
 * the long-running payload. The calls and the launch structure follow
 * nuvio-ps5-homebrew's vendor/control.c and EVO Player's
 * tools/ps5-controllers/launch.c (GPL-3.0-or-later, after John Tornblom's
 * shsrv); both steps were run on a PS5 Pro on 13.42 with probe\lncprobe.c.
 */
#include <stdint.h>
#include <stdio.h>

#ifndef MIRAGE_TITLE_ID
#define MIRAGE_TITLE_ID "PPSA99741"
#endif

typedef struct {
    uint32_t size, user_id, options;
    uint64_t crash_report;
    uint32_t flags;
    char **argv;
} launch_context_t;
_Static_assert(sizeof(launch_context_t) == 40, "launch context must keep the layout the references use");

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *);
int sceUserServiceTerminate(void);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceLaunchApp(const char *, char **, launch_context_t *);

/* -DDYNAMIC_PAD adds one relative relocation, for a build whose PT_DYNAMIC
 * size would otherwise not be a multiple of 16 (see NOTES.md). */
#ifdef DYNAMIC_PAD
static void *volatile dynamic_size_pad = (void *)&dynamic_size_pad;
#endif

int main(void)
{
    launch_context_t context = {0};
    char *args[] = {NULL};
    int owned, user_rc, running, launch = 0;

#ifdef DYNAMIC_PAD
    (void)dynamic_size_pad;
#endif
    owned = sceUserServiceInitialize(NULL) == 0;
    user_rc = sceUserServiceGetForegroundUser(&context.user_id);
    running = sceSystemServiceGetAppIdOfRunningBigApp();
    if (running <= 0 && user_rc == 0)
        launch = sceSystemServiceLaunchApp(MIRAGE_TITLE_ID, args, &context);
    printf("MIRAGE-LAUNCHER user=%d running=%d launch=%d\n", user_rc, running, launch);
    fflush(stdout);
    if (owned)
        sceUserServiceTerminate();
    return 0;
}
