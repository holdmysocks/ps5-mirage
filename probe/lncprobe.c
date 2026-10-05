/*
 * Mirage - probe: which app is running, and (ACTION 2) launching the title.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * For the always-loaded mode: the payload has to know whether the console is
 * on the home screen, and to open the Mirage title when a device connects.
 * The calls and the launch structure are those of two existing payloads:
 * nuvio-ps5-homebrew's vendor/control.c and EVO Player's
 * tools/ps5-controllers/launch.c (both GPL-3.0-or-later, the launch ABI after
 * John Tornblom's shsrv). Linked, as they are, with -lSceUserService
 * -lSceSystemService.
 *
 *   ACTION 1  ask only: the foreground user and the running "big app"
 *   ACTION 2  the same, then launch MIRAGE_TITLE_ID if nothing is running
 *
 * Runs through the ELF loader; every step is printed before it is taken.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#ifndef ACTION
#define ACTION 1
#endif
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
 * size would otherwise not be a multiple of 16 (see NOTES.md, "M4 code"):
 * no ELF with such a table has been run on the console, and none will be. */
#ifdef DYNAMIC_PAD
static void *volatile dynamic_size_pad = (void *)&dynamic_size_pad;
#endif

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

int main(void)
{
    launch_context_t context = {0};
    int owned, rc, running;

#ifdef DYNAMIC_PAD
    (void)dynamic_size_pad; /* a read keeps the variable, and so its relocation, in the ELF */
#endif
    say("lncprobe: action %d, pid %d", ACTION, (int)getpid());

    say("step: sceUserServiceInitialize");
    rc = sceUserServiceInitialize(NULL);
    owned = rc == 0;
    say("  rc 0x%08x%s", (unsigned)rc, owned ? "" : " (already initialised, or refused)");

    say("step: sceUserServiceGetForegroundUser");
    rc = sceUserServiceGetForegroundUser(&context.user_id);
    say("  rc 0x%08x, user 0x%08x", (unsigned)rc, context.user_id);

    say("step: sceSystemServiceGetAppIdOfRunningBigApp");
    running = sceSystemServiceGetAppIdOfRunningBigApp();
    say("  result 0x%08x (%s)", (unsigned)running, running > 0 ? "an app is running" : "no app, or an error code");

#if ACTION >= 2
    if (running > 0) {
        say("RESULT: an app is running, not launching");
    } else if (rc != 0) {
        say("RESULT: no foreground user, not launching");
    } else {
        char *args[] = {NULL};
        say("step: sceSystemServiceLaunchApp(\"%s\")", MIRAGE_TITLE_ID);
        rc = sceSystemServiceLaunchApp(MIRAGE_TITLE_ID, args, &context);
        say("  rc 0x%08x", (unsigned)rc);
        say("RESULT: launch %s", rc < 0 ? "refused" : "accepted");
    }
#else
    say("RESULT: asked, nothing launched");
#endif

    if (owned) {
        say("step: sceUserServiceTerminate");
        sceUserServiceTerminate();
    }
    return 0;
}
