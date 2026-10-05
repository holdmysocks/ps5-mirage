/*
 * Mirage - AudioOut probe for the PS5 (can a payload play sound?).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs as a payload through the ELF loader; every step is printed to the
 * loader socket before it runs. The library is loaded with dlopen, not linked,
 * so the process starts and logs even if the module cannot be loaded.
 *
 * STAGE selects how far the probe goes, one new operation per run:
 *   1  dlopen libSceAudioOut.sprx and look up the four functions
 *   2  + sceAudioOutInit
 *   3  + sceAudioOutOpen (system user, main port, 256 frames, 48 kHz, S16
 *        stereo) and sceAudioOutClose
 *   4  + one second of a quiet 440 Hz tone through sceAudioOutOutput
 *
 * Variants: -DAUDIO_PORT_MAIN=<type> (1 BGM, 2 voice, 3 personal, 4 pad speaker,
 * 127 aux), -DAUDIO_USER_FOREGROUND (the signed-in user instead of 0xff),
 * -DTONE_AMPLITUDE, -DTONE_SECONDS.
 *
 * A watchdog thread kills the process after WATCHDOG_SECONDS, so a call that
 * never returns cannot leave it behind.
 */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef STAGE
#define STAGE 1
#endif

#define WATCHDOG_SECONDS 15
#define AUDIO_USER_SYSTEM 0xff
#ifndef AUDIO_PORT_MAIN
#define AUDIO_PORT_MAIN 0 /* build with -DAUDIO_PORT_MAIN=1 to try the BGM port type */
#endif
#define AUDIO_FORMAT_S16_STEREO 1
#define GRAIN_FRAMES 256
#define SAMPLE_RATE 48000
#define TONE_HZ 440.0
#ifndef TONE_AMPLITUDE
#define TONE_AMPLITUDE 1600 /* about -26 dBFS */
#endif
#ifndef TONE_SECONDS
#define TONE_SECONDS 1
#endif
#define TONE_GRAINS (TONE_SECONDS * SAMPLE_RATE / GRAIN_FRAMES)

static int (*audio_init)(void);
static int (*audio_open)(int32_t user, int32_t type, int32_t index, uint32_t frames, uint32_t rate,
                         uint32_t format);
static int (*audio_output)(int32_t handle, const void *samples);
static int (*audio_close)(int32_t handle);
static int32_t audio_user = AUDIO_USER_SYSTEM;

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

static void *watchdog_main(void *arg)
{
    static const char msg[] = "audioprobe: watchdog expired, killing the process\n";
    (void)arg;
    sleep(WATCHDOG_SECONDS);
    write(1, msg, sizeof(msg) - 1);
    kill(getpid(), SIGKILL);
    return NULL;
}

static void *lookup(void *module, const char *name)
{
    void *address = dlsym(module, name);
    say("  %s: %s", name, address ? "found" : "missing");
    return address;
}

int main(void)
{
    pthread_t watchdog;
    void *module;
    int rc;

    say("audioprobe: stage %d, pid %d", STAGE, (int)getpid());
    say("step: start a %d s watchdog", WATCHDOG_SECONDS);
    if (pthread_create(&watchdog, NULL, watchdog_main, NULL) != 0) {
        say("RESULT: no watchdog, not continuing");
        return 1;
    }

    say("step: dlopen libSceAudioOut.sprx");
    module = dlopen("libSceAudioOut.sprx", RTLD_LAZY);
    if (!module) {
        const char *why = dlerror();
        say("RESULT: dlopen failed: %s", why ? why : "?");
        return 1;
    }
    say("  loaded");

    say("step: look up the functions");
    audio_init = lookup(module, "sceAudioOutInit");
    audio_open = lookup(module, "sceAudioOutOpen");
    audio_output = lookup(module, "sceAudioOutOutput");
    audio_close = lookup(module, "sceAudioOutClose");
    if (!audio_init || !audio_open || !audio_output || !audio_close) {
        say("RESULT: a function is missing");
        return 1;
    }

#if STAGE >= 2
    say("step: sceAudioOutInit");
    rc = audio_init();
    say("  rc 0x%08x", (unsigned)rc);
#endif

#ifdef AUDIO_USER_FOREGROUND
    {
        /* Use the signed-in foreground user instead of the system user id. */
        void *user_module;
        int (*user_initialize)(const void *params);
        int (*user_get_foreground)(int32_t *user);

        say("step: dlopen libSceUserService.sprx");
        user_module = dlopen("libSceUserService.sprx", RTLD_LAZY);
        if (!user_module) {
            say("RESULT: dlopen failed");
            return 1;
        }
        user_initialize = lookup(user_module, "sceUserServiceInitialize");
        user_get_foreground = lookup(user_module, "sceUserServiceGetForegroundUser");
        if (!user_initialize || !user_get_foreground) {
            say("RESULT: a function is missing");
            return 1;
        }
        say("step: sceUserServiceInitialize(NULL)");
        rc = user_initialize(NULL);
        say("  rc 0x%08x", (unsigned)rc);
        say("step: sceUserServiceGetForegroundUser");
        rc = user_get_foreground(&audio_user);
        say("  rc 0x%08x, user 0x%08x", (unsigned)rc, (unsigned)audio_user);
        if (rc != 0) {
            say("RESULT: no foreground user");
            return 1;
        }
    }
#endif

#if STAGE >= 3
    {
        int handle;

        say("step: sceAudioOutOpen(user 0x%x, port %d, %d frames, %d Hz, format %d)", (unsigned)audio_user,
            AUDIO_PORT_MAIN, GRAIN_FRAMES, SAMPLE_RATE, AUDIO_FORMAT_S16_STEREO);
        handle = audio_open(audio_user, AUDIO_PORT_MAIN, 0, GRAIN_FRAMES, SAMPLE_RATE,
                            AUDIO_FORMAT_S16_STEREO);
        say("  handle 0x%08x", (unsigned)handle);
        if (handle <= 0) {
            say("RESULT: open failed");
            return 1;
        }

#if STAGE >= 4
        {
            static int16_t grain[GRAIN_FRAMES * 2];
            const double step = 2.0 * 3.14159265358979323846 * TONE_HZ / SAMPLE_RATE;
            double phase = 0.0, started, worst = 0.0;
            int failures = 0;

            say("step: %d grains of a %d Hz tone, amplitude %d of 32767", TONE_GRAINS, (int)TONE_HZ,
                TONE_AMPLITUDE);
            started = now_s();
            for (int g = 0; g < TONE_GRAINS; ++g) {
                /* Fade the first and last grains so the tone does not click. */
                double gain = 1.0;
                double before, took;
                if (g < 8)
                    gain = g / 8.0;
                else if (g >= TONE_GRAINS - 8)
                    gain = (TONE_GRAINS - 1 - g) / 8.0;
                for (int i = 0; i < GRAIN_FRAMES; ++i) {
                    int16_t sample = (int16_t)(sin(phase) * TONE_AMPLITUDE * gain);
                    grain[2 * i] = sample;
                    grain[2 * i + 1] = sample;
                    phase += step;
                }
                before = now_s();
                rc = audio_output(handle, grain);
                took = now_s() - before;
                if (took > worst)
                    worst = took;
                if (rc < 0 && failures++ == 0)
                    say("  grain %d: rc 0x%08x", g, (unsigned)rc);
                if (g == 0)
                    say("  first grain: rc 0x%08x, %.1f ms", (unsigned)rc, took * 1000.0);
            }
            say("  %d grains in %.3f s (expected about %d), worst call %.1f ms, %d failed", TONE_GRAINS,
                now_s() - started, TONE_SECONDS, worst * 1000.0, failures);
            say("step: drain (output NULL)");
            rc = audio_output(handle, NULL);
            say("  rc 0x%08x", (unsigned)rc);
        }
#endif

        say("step: sceAudioOutClose");
        rc = audio_close(handle);
        say("  rc 0x%08x", (unsigned)rc);
    }
#endif

    say("RESULT: stage %d finished", STAGE);
    return 0;
}
