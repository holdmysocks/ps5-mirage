/*
 * Mirage - AirPlay receiver for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The payload half: advertises the receiver, runs the AirPlay protocol,
 * decodes audio and hands 48 kHz PCM to the title over loopback (pcm_link.h).
 * A loader payload cannot be heard, so the title does the output. Runs
 * through the ELF loader, so stdout is the loader's TCP socket.
 *
 * MIRAGE_LINK_TEST_TONE builds a variant with no AirPlay at all: it only
 * serves beeps on the link, to test the link and the title by themselves.
 *
 * Test builds set MIRAGE_RUN_SECONDS: the process then stops itself after
 * that long, whatever state it is in.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio_pipeline.h"
#include "hls.h"
#include "launcher.h"
#include "log_server.h"
#include "dnssd.h"
#include "logger.h"
#include "pcm_link.h"
#include "pcm_server.h"
#include "raop.h"
#include "stream.h"

#ifndef MIRAGE_VERSION
#define MIRAGE_VERSION "0.0.0-dev"
#endif
#ifndef MIRAGE_NAME
#define MIRAGE_NAME "Mirage"
#endif
#ifndef MIRAGE_RUN_SECONDS
#define MIRAGE_RUN_SECONDS 0
#endif
/* The payload stays for as long as the console is up, with or without a
 * title: started by the title or by an autoloader, it advertises at once and
 * opens the title itself when a device connects. It stops when a newer copy
 * asks it to (the ELF was sent again) or, in a test build, after
 * MIRAGE_RUN_SECONDS.
 *
 * How long to keep asking the Remote Play service for the console's name at
 * start (straight after boot it may not be answering yet), and how long to
 * leave between two attempts to open the title. */
#ifndef MIRAGE_NAME_WAIT_SECONDS
#define MIRAGE_NAME_WAIT_SECONDS 20
#endif
#ifndef MIRAGE_LAUNCH_INTERVAL_SECONDS
#define MIRAGE_LAUNCH_INTERVAL_SECONDS 20
#endif
/* Whether casting ("play this URL", HLS) is advertised. */
#ifndef MIRAGE_HLS
#define MIRAGE_HLS 1
#endif
#ifndef MIRAGE_LOG_LEVEL
#define MIRAGE_LOG_LEVEL LOGGER_INFO
#endif

/* The AirPlay device id is the console's own network address, so that it is
 * the same at every start and different on every console. This locally
 * administered one stands in if the system does not give it. */
static char hw_addr[6] = {0x02, 0x4d, 0x49, 0x52, 0x41, 0x47};
static char device_id[18] = "02:4D:49:52:41:47";

/* Where the receiver's key pair is kept, so that it too survives a restart.
 * A loader payload may not be allowed to write there; then there is no file
 * and a new key is made at each start, as before. */
#define MIRAGE_DATA_DIR "/data/mirage"
#define MIRAGE_KEY_FILE MIRAGE_DATA_DIR "/receiver.pem"
/* Settings, read at the start; written with the defaults if it is not there. */
#define MIRAGE_CONFIG_FILE MIRAGE_DATA_DIR "/mirage.conf"
/* The devices that have paired with a code: one line each, key first. */
#define MIRAGE_PAIRED_FILE MIRAGE_DATA_DIR "/paired.txt"
#define MAX_PAIRED 64
/* A pairing code stays on screen this long if nobody enters it. */
#define PIN_SHOWN_SECONDS 90

/* How long to wait for the title's hello (the console's name) once a title
 * has connected, before advertising under the plain name. */
#define MIRAGE_HELLO_WAIT_MS 3000

int sceNetGetMacAddress(unsigned char address[6], int flags);

static pthread_mutex_t say_lock = PTHREAD_MUTEX_INITIALIZER;
static raop_t *the_raop; /* for the callbacks that have to call back into the library */
static atomic_ulong audio_packets, audio_bytes, video_packets, video_bytes;
static atomic_int connections;

/* PIN pairing (off unless the settings file says "pin = on"). */
static int pin_required;
static pthread_mutex_t pairing_lock = PTHREAD_MUTEX_INITIALIZER;
static char paired[MAX_PAIRED][64]; /* public keys as the library gives them; guarded by pairing_lock */
static int paired_count;
static char current_pin[PCM_LINK_PIN_MAX + 1]; /* the code on screen, "" for none; guarded by pairing_lock */
static atomic_long pin_shown_at;    /* CLOCK_MONOTONIC seconds, 0 while no code is shown */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    /* One line: to the loader socket (if anyone still reads it) and to the
     * log kept in memory, which log_server hands out over the network. */
    static char line[2048];
    va_list ap;
    int length;

    pthread_mutex_lock(&say_lock);
    va_start(ap, fmt);
    length = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (length < 0)
        length = 0;
    if (length > (int)sizeof(line) - 2)
        length = (int)sizeof(line) - 2;
    line[length++] = '\n';
    fwrite(line, 1, (size_t)length, stdout);
    fflush(stdout);
    log_server_append(line, (size_t)length);
    pthread_mutex_unlock(&say_lock);
}

/* ---- scheduling ---------------------------------------------------------
 * Payload threads start in the FIFO class at priority 700, where one busy
 * thread can freeze the console. Move to time-sharing before any thread is
 * created; new threads inherit it. Sony only accepts priorities 256..767. */
#define SYS_rtprio_thread 466
#define RTP_LOOKUP 0
#define RTP_SET 1
#define RTP_PRIO_NORMAL 3
#define PS5_PRIO_LOWEST 767

struct rtprio {
    unsigned short type;
    unsigned short prio;
};

static int leave_realtime_class(void)
{
    struct rtprio want = {RTP_PRIO_NORMAL, PS5_PRIO_LOWEST};
    struct rtprio after = {0, 0};

    syscall(SYS_rtprio_thread, RTP_SET, 0, &want);
    syscall(SYS_rtprio_thread, RTP_LOOKUP, 0, &after);
    say("sched: class %u priority %u", after.type, after.prio);
    return after.type == want.type && after.prio == want.prio;
}

/* ---- callbacks ---------------------------------------------------------- */

static void cb_conn_init(void *cls)
{
    (void)cls;
    say("conn: open (%d)", atomic_fetch_add(&connections, 1) + 1);
}

static void cb_conn_destroy(void *cls)
{
    (void)cls;
    say("conn: closed (%d)", atomic_fetch_sub(&connections, 1) - 1);
    pcm_server_send(PCM_LINK_STOP);
}

static void cb_conn_reset(void *cls, int reason)
{
    (void)cls;
    say("conn: reset, reason %d", reason);
}

static void cb_conn_feedback(void *cls)
{
    (void)cls;
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data)
{
    (void)cls;
    (void)ntp;
    unsigned long n = atomic_fetch_add(&audio_packets, 1) + 1;
    atomic_fetch_add(&audio_bytes, (unsigned long)data->data_len);
    /* The stream's timeline is its RTP clock: 44100 ticks a second, one tick
     * per sample, unwrapped to 64 bits. (UxPlay's ntp_time_local is not
     * usable for this: on the console it stayed near zero for a whole
     * stream.) Called from the one RTP thread, so plain statics do. */
    static uint32_t last_rtp;
    static int64_t ticks;
    static int64_t first_ticks, first_arrival_ns;
    struct timespec now;
    int64_t now_ns, time_ns;

    if (n == 1) {
        ticks = 0;
    } else {
        ticks += (int32_t)(data->rtp_time - last_rtp);
    }
    last_rtp = data->rtp_time;
    time_ns = ticks * 1000000000LL / 44100;

    /* Slip: how much later than the first packet this one arrived, measured
     * against the timeline. Positive eats into the title's start delay.
     * Logged densely at the start, where the stream is uneven. */
    clock_gettime(CLOCK_MONOTONIC, &now);
    now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    if (n == 1) {
        first_ticks = time_ns;
        first_arrival_ns = now_ns;
    }
    if (n <= 5 || (n <= 500 && n % 50 == 0) || n % 500 == 0)
        /* remote: UxPlay's ntp_time_remote, logged to see whether it could put
         * audio on the same timeline as the video (which uses it). */
        say("audio: packet %lu, ct %u, %d bytes, first byte 0x%02x, seq %u, rtp %u, slip %lld ms, remote %llu ms", n,
            data->ct, data->data_len, data->data_len > 0 ? data->data[0] : 0, data->seqnum, data->rtp_time,
            (long long)((now_ns - first_arrival_ns) - (time_ns - first_ticks)) / 1000000LL,
            (unsigned long long)(data->ntp_time_remote / 1000000ULL));
    /* Mirroring (AAC-ELD) sound is stamped with the sender's clock, the same
     * one the pictures carry, so the title can show each picture with its
     * sound; seen on the console to advance with the samples (2026-10-05).
     * Audio-only streams stay on the RTP timeline, which is known good. */
    {
        const int live = data->ct == 8 && data->ntp_time_remote != 0;
        audio_pipeline_packet(data->ct, data->data, data->data_len, live ? data->ntp_time_remote : (uint64_t)time_ns,
                              live);
    }
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data)
{
    (void)cls;
    (void)ntp;
    unsigned long n = atomic_fetch_add(&video_packets, 1) + 1;
    atomic_fetch_add(&video_bytes, (unsigned long)data->data_len);
    /* The times are logged as well as sent: whether ntp_time_remote advances
     * with the pictures has to be seen on the console, not assumed. */
    if (n <= 5 || n % 300 == 0)
        say("video: unit %lu, %s, %d NAL, %d bytes, remote time %llu ms, local %llu ms", n,
            data->is_h265 ? "h265" : "h264", data->nal_count, data->data_len,
            (unsigned long long)(data->ntp_time_remote / 1000000ULL),
            (unsigned long long)(data->ntp_time_local / 1000000ULL));
    if (data->data_len > 0)
        pcm_server_send_video((int64_t)data->ntp_time_remote, data->is_h265, data->nal_count, data->data,
                              (uint32_t)data->data_len);
}

static void cb_audio_flush(void *cls)
{
    (void)cls;
    say("audio: flush");
    audio_pipeline_flush();
}

static void cb_video_flush(void *cls)
{
    (void)cls;
}

static void cb_video_pause(void *cls)
{
    (void)cls;
}

static void cb_video_resume(void *cls)
{
    (void)cls;
}

static double cb_audio_set_client_volume(void *cls)
{
    (void)cls;
    return 0.0; /* dB, full volume */
}

static void cb_audio_set_volume(void *cls, float volume)
{
    (void)cls;
    say("audio: volume %.2f dB", (double)volume);
    audio_pipeline_set_volume(volume);
}

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *using_screen,
                                bool *is_media, uint64_t *audio_format)
{
    (void)cls;
    say("audio: format ct %u, %u samples per frame, screen %d, media %d, format 0x%llx", *ct, *spf,
        *using_screen, *is_media, (unsigned long long)*audio_format);
}

static void cb_video_report_size(void *cls, float *width_source, float *height_source, float *width,
                                 float *height)
{
    (void)cls;
    say("video: source %.0fx%.0f, picture %.0fx%.0f", (double)*width_source, (double)*height_source,
        (double)*width, (double)*height);
    pcm_server_send_video_size((uint32_t)*width_source, (uint32_t)*height_source, (uint32_t)*width,
                               (uint32_t)*height);
}

static void cb_video_reset(void *cls, reset_type_t type)
{
    (void)cls;
    say("video: reset, type %d", (int)type);
    pcm_server_send(PCM_LINK_VIDEO_RESET);
    /* What UxPlay's own front end does for casts on these resets: the
     * library keeps per-cast state that it does not drop by itself. */
    if (the_raop && (type == RESET_TYPE_NOHOLD || type == RESET_TYPE_HLS_SHUTDOWN)) {
        raop_destroy_airplay_video(the_raop, -1);
        if (type == RESET_TYPE_HLS_SHUTDOWN)
            raop_remove_hls_connections(the_raop);
    }
    if (type == RESET_TYPE_NOHOLD || type == RESET_TYPE_HLS_SHUTDOWN || type == RESET_TYPE_HLS_EOS)
        hls_reset();
}

static int cb_video_set_codec(void *cls, video_codec_t codec)
{
    (void)cls;
    say("video: codec %d", (int)codec);
    return 0;
}

/* ---- opening the title -------------------------------------------------- */

static atomic_long last_launch_attempt; /* CLOCK_MONOTONIC seconds, 0 for never */

/* Runs on its own thread: the helper takes seconds to answer. The argument
 * is the connecting device's name, to be freed here. */
static void *open_title_main(void *arg)
{
    char *device = arg;
    launcher_result_t result;

    if (launcher_run(&result) != 0) {
        say("title: the launcher helper did not answer; the title has to be opened by hand");
    } else if (result.launch > 0) {
        say("title: nothing was running, opened the Mirage title (app id 0x%x)", (unsigned)result.launch);
    } else if (result.running > 0) {
        /* Something is being played or watched: say so and leave it alone. */
        char pin[sizeof(current_pin)];

        pthread_mutex_lock(&pairing_lock);
        snprintf(pin, sizeof(pin), "%s", current_pin);
        pthread_mutex_unlock(&pairing_lock);
        if (pin[0] != '\0') { /* the code cannot be shown by a title: it goes in the notification */
            say("title: app 0x%x is running, not opening the title; the pairing code goes in a notification",
                (unsigned)result.running);
            launcher_notify("Mirage pairing code: %s\nEnter it on the device that asks for it.", pin);
            free(device);
            return NULL;
        }
        say("title: app 0x%x is running, not opening the title; notifying", (unsigned)result.running);
        launcher_notify("%s wants to AirPlay to this console.\nOpen Mirage to see and hear it.",
                        device && device[0] ? device : "A device");
    } else {
        say("title: could not open the title (user rc %d, running %d, launch 0x%x)", result.user, result.running,
            (unsigned)result.launch);
    }
    free(device);
    return NULL;
}

/* A device is connecting. If no title is there to play what it sends, open
 * one, but not more often than every MIRAGE_LAUNCH_INTERVAL_SECONDS. */
static void open_title_if_needed(const char *device)
{
    struct timespec now;
    pthread_attr_t attr;
    pthread_t thread;
    long previous;

    if (pcm_server_connected())
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    previous = atomic_load(&last_launch_attempt);
    if (previous != 0 && now.tv_sec - previous < MIRAGE_LAUNCH_INTERVAL_SECONDS)
        return;
    atomic_store(&last_launch_attempt, (long)now.tv_sec);
    say("title: no title is connected; asking the launcher helper");
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, open_title_main, strdup(device ? device : "")) != 0)
        say("title: could not start the launcher thread");
    pthread_attr_destroy(&attr);
}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit)
{
    (void)cls;
    say("client: \"%s\" (%s, %s) asks to connect", name ? name : "?", model ? model : "?",
        deviceid ? deviceid : "?");
    *admit = true;
    open_title_if_needed(name);
}

/* ---- PIN pairing --------------------------------------------------------- */

static void set_pin(const char *pin)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&pairing_lock);
    snprintf(current_pin, sizeof(current_pin), "%s", pin ? pin : "");
    pthread_mutex_unlock(&pairing_lock);
    atomic_store(&pin_shown_at, pin && pin[0] ? (long)now.tv_sec : 0);
    pcm_server_set_pin(pin);
}

/* The library has made a code and waits for the device to send it back. The
 * code itself is not logged: the log can be read by anyone on the network. */
static void cb_display_pin(void *cls, char *pin)
{
    (void)cls;
    say("pairing: a device has to enter a code; showing it");
    set_pin(pin);
    open_title_if_needed("A device");
}

/* A device has entered the code: it is remembered and not asked again. */
static void cb_register_client(void *cls, const char *device_id, const char *client_pk, const char *client_name)
{
    int known = 0, stored = 0;

    (void)cls;
    if (!client_pk || strlen(client_pk) >= sizeof(paired[0]))
        return;
    pthread_mutex_lock(&pairing_lock);
    for (int i = 0; i < paired_count && !known; ++i)
        known = strcmp(paired[i], client_pk) == 0;
    if (!known && paired_count < MAX_PAIRED) {
        strcpy(paired[paired_count++], client_pk);
        stored = 1;
    }
    pthread_mutex_unlock(&pairing_lock);
    if (stored) {
        FILE *file = fopen(MIRAGE_PAIRED_FILE, "a");
        if (file) {
            fprintf(file, "%s,%s,%s\n", client_pk, device_id ? device_id : "", client_name ? client_name : "");
            fclose(file);
        } else {
            say("pairing: cannot write %s (errno %d): this device will be asked again after a restart",
                MIRAGE_PAIRED_FILE, errno);
        }
    }
    say("pairing: \"%s\" (%s) %s", client_name ? client_name : "?", device_id ? device_id : "?",
        known ? "was paired already" : stored ? "is now paired" : "paired, but the list is full");
    set_pin("");
}

static bool cb_check_register(void *cls, const char *client_pk)
{
    int known = 0;

    (void)cls;
    pthread_mutex_lock(&pairing_lock);
    for (int i = 0; client_pk && i < paired_count && !known; ++i)
        known = strcmp(paired[i], client_pk) == 0;
    pthread_mutex_unlock(&pairing_lock);
    say("pairing: a returning device is %s", known ? "on the list" : "not on the list; it has to pair again");
    return known;
}

/* Reads the settings file, or writes one with the defaults, and loads the
 * list of paired devices. */
static void read_settings(void)
{
    char line[256];
    FILE *file = fopen(MIRAGE_CONFIG_FILE, "r");

    if (file) {
        while (fgets(line, sizeof(line), file)) {
            char key[32] = "", value[32] = "";
            if (line[0] == '#' || sscanf(line, " %31[A-Za-z_] = %31s", key, value) != 2)
                continue;
            if (strcasecmp(key, "pin") == 0)
                pin_required = strcasecmp(value, "on") == 0 || strcasecmp(value, "yes") == 0 ||
                               strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0;
        }
        fclose(file);
    } else if ((file = fopen(MIRAGE_CONFIG_FILE, "w")) != NULL) {
        fputs("# Mirage settings. Read when the payload starts.\n"
              "#\n"
              "# pin = on    a device has to enter a 4 digit code shown on the TV the\n"
              "#             first time it connects; it is remembered after that\n"
              "#             (the list is paired.txt in this folder)\n"
              "# pin = off   any device on the network can connect\n"
              "pin = off\n",
              file);
        fclose(file);
        say("settings: wrote %s with the defaults", MIRAGE_CONFIG_FILE);
    }
    if (pin_required && (file = fopen(MIRAGE_PAIRED_FILE, "r")) != NULL) {
        while (paired_count < MAX_PAIRED && fgets(line, sizeof(line), file)) {
            const size_t key_length = strcspn(line, ",\r\n");
            if (key_length > 0 && key_length < sizeof(paired[0])) {
                memcpy(paired[paired_count], line, key_length);
                paired[paired_count++][key_length] = '\0';
            }
        }
        fclose(file);
    }
    say("settings: PIN pairing %s%s", pin_required ? "on" : "off",
        pin_required ? "" : " (pin = on in " MIRAGE_CONFIG_FILE " turns it on)");
    if (pin_required)
        say("settings: %d paired device(s)", paired_count);
}

/* Casting ("play this URL"): see hls.h. */
static void cb_on_video_play(void *cls, const char *location, const float start_position)
{
    (void)cls;
    hls_on_play(location, start_position);
    open_title_if_needed("A device");
}

static void cb_on_video_scrub(void *cls, const float position)
{
    (void)cls;
    hls_on_scrub(position);
}

static void cb_on_video_rate(void *cls, const float rate)
{
    (void)cls;
    hls_on_rate(rate);
}

static void cb_on_video_stop(void *cls)
{
    (void)cls;
    hls_on_stop();
}

static void cb_on_video_acquire_playback_info(void *cls, playback_info_t *info)
{
    (void)cls;
    hls_on_playback_info(info);
}

static float cb_on_video_playlist_remove(void *cls)
{
    (void)cls;
    return hls_on_playlist_remove();
}

static void cb_log(void *cls, int level, const char *msg)
{
    (void)cls;
    /* The library prints the pairing code it has just made. The log is
     * served to the network, so that line is not kept (seen in i2-run1.log). */
    if (msg && strstr(msg, "PIN = ")) {
        say("uxplay[%d]: (a line with the pairing code, not logged)", level);
        return;
    }
    say("uxplay[%d]: %s", level, msg);
}

/* ---- link test ---------------------------------------------------------- */

#ifdef MIRAGE_LINK_TEST_TONE
/* One extra relative relocation. Without it this variant's PT_DYNAMIC segment
 * (which the SDK's linker script stretches over the relocation table) has a
 * size that is not a multiple of 16, and readelf calls the table invalid.
 * Every ELF run on the console so far had a multiple of 16; keep it that way
 * instead of finding out what the loader does otherwise. tools\build.ps1
 * refuses to produce an ELF that fails this check. */
static void *volatile dynamic_size_pad = (void *)&dynamic_size_pad;

/* Serves a 2 s beep every 3 s on the link, paced by the clock, 10 ms at a time. */
static void serve_test_tone(int seconds)
{
    enum { CHUNK = PCM_LINK_SAMPLE_RATE / 100 };
    static int16_t chunk[CHUNK * PCM_LINK_CHANNELS];
    /* 440 Hz at 48 kHz as a rotation per sample. */
    const float c = 0.99834180f, s = 0.05756403f;
    float x = 1.0f, y = 0.0f;
    struct timespec next;
    unsigned long sent = 0;

    clock_gettime(CLOCK_REALTIME, &next);
    const long long tone_start_ns = (long long)next.tv_sec * 1000000000LL + next.tv_nsec;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (long tick = 0; tick < (long)seconds * 100; ++tick) {
        const long in_cycle = tick % 300; /* 3 s cycle: 2 s tone, 1 s silence */
        float gain = 0.0f;
        if (in_cycle < 200) {
            gain = 8200.0f;
            if (in_cycle < 2)
                gain *= (float)in_cycle / 2.0f;
            else if (in_cycle >= 198)
                gain *= (float)(199 - in_cycle) / 2.0f;
        }
        if (in_cycle == 0) {
            x = 1.0f;
            y = 0.0f;
        }
        for (int i = 0; i < CHUNK; ++i) {
            const int16_t sample = (int16_t)(y * gain);
            const float nx = x * c - y * s;
            chunk[2 * i] = sample;
            chunk[2 * i + 1] = sample;
            y = x * s + y * c;
            x = nx;
        }
        if (pcm_server_connected()) {
            /* Stamp each chunk with its place on a 10 ms grid that starts at
             * the first one, as a real stream's packets are stamped. */
            pcm_server_send_audio(tone_start_ns + tick * 10000000LL, 0, chunk, sizeof(chunk));
            ++sent;
        }
        if (tick % 1000 == 999)
            say("status: %ld s, title %s, %lu chunks sent", (tick + 1) / 100,
                pcm_server_connected() ? "connected" : "not connected", sent);
        /* Sleep until the next 10 ms mark. Relative nanosleep on purpose: the
         * console's kernel is FreeBSD 9 based and may lack clock_nanosleep. */
        next.tv_nsec += 10000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        {
            struct timespec now, wait;
            clock_gettime(CLOCK_MONOTONIC, &now);
            wait.tv_sec = next.tv_sec - now.tv_sec;
            wait.tv_nsec = next.tv_nsec - now.tv_nsec;
            if (wait.tv_nsec < 0) {
                wait.tv_nsec += 1000000000L;
                wait.tv_sec -= 1;
            }
            if (wait.tv_sec < 0) {
                next = now; /* fell behind: do not try to catch up in a burst */
                wait.tv_sec = 0;
                wait.tv_nsec = 1000000L;
            }
            nanosleep(&wait, NULL);
        }
    }
}
#endif

/* ---- identity ----------------------------------------------------------- */

static void read_mac_address(void)
{
    unsigned char mac[6] = {0};
    int rc = sceNetGetMacAddress(mac, 0);
    int all_zero = 1;

    for (int i = 0; i < 6; ++i)
        all_zero = all_zero && mac[i] == 0;
    if (rc != 0 || all_zero) {
        say("identity: no MAC address from the system (rc 0x%08x), using the stand-in %s", (unsigned)rc, device_id);
        return;
    }
    memcpy(hw_addr, mac, 6);
    snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    say("identity: device id %s", device_id);
}

/* The console's name as its Remote Play service gives it: the reply to a
 * discovery request on loopback UDP 9302 has a "host-name:" line (seen on
 * this console by the tailscale project's rpprobe). Works without a title,
 * needs no system library, and only answers while Remote Play is enabled in
 * the console's settings. Returns 1 and fills name on success. */
static int console_name_from_remote_play(char *name, size_t size)
{
    static const char request[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
    static const char key[] = "host-name:";
    struct sockaddr_in addr;
    char reply[1024];
    ssize_t n = -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9302);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sendto(fd, request, sizeof(request) - 1, 0, (struct sockaddr *)&addr, sizeof(addr)) > 0) {
        for (int tries = 0; tries < 30 && n < 0; ++tries) { /* up to 1.5 s, never blocking */
            n = recv(fd, reply, sizeof(reply) - 1, MSG_DONTWAIT);
            if (n < 0)
                usleep(50000);
        }
    }
    close(fd);
    if (n <= 0)
        return 0;
    reply[n] = '\0';
    {
        char *line = strstr(reply, key);
        size_t length;
        if (!line)
            return 0;
        line += sizeof(key) - 1;
        while (*line == ' ')
            ++line;
        length = strcspn(line, "\r\n");
        if (length == 0 || size == 0)
            return 0;
        if (length > size - 1)
            length = size - 1;
        memcpy(name, line, length);
        name[length] = '\0';
    }
    return 1;
}

/* "Mirage - <console name>", or plain "Mirage" without a usable name. The
 * console's name is whatever its owner typed: keep what a service name can
 * carry (printable ASCII, no dots), and bound its length. */
static void build_name(char *name, size_t size, const char *console)
{
    char clean[41];
    size_t n = 0;

    for (const char *p = console; *p && n < sizeof(clean) - 1; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c < 0x7f && c != '.')
            clean[n++] = (char)c;
    }
    while (n > 0 && clean[n - 1] == ' ')
        --n;
    clean[n] = '\0';
    if (n > 0)
        snprintf(name, size, "%s - %s", MIRAGE_NAME, clean);
    else
        snprintf(name, size, "%s", MIRAGE_NAME);
}

/* Returns the key file to use, or "" if the folder cannot be written. */
static const char *prepare_key_file(void)
{
    FILE *probe;

    (void)mkdir(MIRAGE_DATA_DIR, 0777); /* fails harmlessly if it exists */
    probe = fopen(MIRAGE_KEY_FILE, "r");
    if (probe) {
        fclose(probe);
        say("identity: using the key in %s", MIRAGE_KEY_FILE);
        return MIRAGE_KEY_FILE;
    }
    /* No key yet: can one be written? UxPlay creates it if the path is given. */
    probe = fopen(MIRAGE_DATA_DIR "/.probe", "w");
    if (!probe) {
        say("identity: cannot write %s (errno %d): no key file, a new key each start", MIRAGE_DATA_DIR, errno);
        return "";
    }
    fclose(probe);
    remove(MIRAGE_DATA_DIR "/.probe");
    say("identity: a key will be written to %s", MIRAGE_KEY_FILE);
    return MIRAGE_KEY_FILE;
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    char name[96] = MIRAGE_NAME;
    char console[PCM_LINK_HELLO_MAX + 1] = "";
    const char *key_file;
    raop_callbacks_t cbs;
    unsigned short tcp[2] = {0, 0};
    unsigned short udp[3] = {0, 0, 0};
    unsigned short port;
    dnssd_t *dnssd;
    raop_t *raop;
    int error = 0;

    say("mirage %s: pid %d, name \"%s\", run limit %d s", MIRAGE_VERSION, (int)getpid(), MIRAGE_NAME,
        MIRAGE_RUN_SECONDS);

    say("step: ignore SIGPIPE");
    signal(SIGPIPE, SIG_IGN);

    say("step: leave the realtime scheduling class");
    if (!leave_realtime_class()) {
        say("RESULT: could not leave the realtime class, not starting");
        return 1;
    }

    say("step: start the link to the title");
    if (pcm_server_start(say) != 0) {
        say("RESULT: the link did not start");
        return 1;
    }
    if (log_server_start() == 0)
        say("log: readable on TCP port %d", LOG_SERVER_PORT);
    else
        say("log: port %d could not be opened, the log is not served", LOG_SERVER_PORT);

#ifdef MIRAGE_LINK_TEST_TONE
    (void)dynamic_size_pad;
    say("ready: link test, beeps for %d s, no AirPlay", MIRAGE_RUN_SECONDS);
    serve_test_tone(MIRAGE_RUN_SECONDS > 0 ? MIRAGE_RUN_SECONDS : 60);
    say("step: stop the link");
    pcm_server_stop();
    say("RESULT: link test finished");
    return 0;
#endif

    say("step: open the audio decoder");
    if (audio_pipeline_init(say) != 0) {
        say("RESULT: the audio decoder did not start");
        return 1;
    }

    ntp_global_init();

    /* The receiver is advertised under the console's name. The Remote Play
     * service gives it without any title; straight after boot it may need a
     * moment, so it is asked for a while. A title that is already there may
     * also have said it. */
    say("step: find the console's name");
    for (int waited = 0; waited < MIRAGE_NAME_WAIT_SECONDS && console[0] == '\0'; waited += 2) {
        if (console_name_from_remote_play(console, sizeof(console))) {
            say("identity: Remote Play discovery says \"%s\"", console);
        } else if (pcm_server_hello(console, sizeof(console)) && console[0] != '\0') {
            say("identity: the title says \"%s\"", console);
        } else if (pcm_server_quit_requested()) {
            break;
        } else {
            usleep(500000); /* the query itself already waited 1.5 s */
        }
    }
    if (console[0] == '\0')
        say("identity: no console name in %d s (is Remote Play enabled?)", MIRAGE_NAME_WAIT_SECONDS);
    build_name(name, sizeof(name), console);
    say("identity: console name \"%s\", advertising as \"%s\"", console, name);
    pcm_server_set_name(name);
    read_mac_address();
    key_file = prepare_key_file();
    read_settings();

    say("step: dnssd_init");
    dnssd = dnssd_init(name, (int)strlen(name), hw_addr, sizeof(hw_addr), pin_required ? 1 : 0, &error);
    if (!dnssd || error) {
        say("RESULT: dnssd_init failed, error %d", error);
        return 1;
    }
    /* Audio only for now: no HLS, no HEVC. Mirroring stays advertised as in
     * UxPlay's default set; video that arrives is counted and dropped. */
    dnssd_set_airplay_features(dnssd, 0, MIRAGE_HLS); /* AirPlay video */
    dnssd_set_airplay_features(dnssd, 4, MIRAGE_HLS); /* HTTP live streaming */
    dnssd_set_airplay_features(dnssd, 42, 0);
    dnssd_set_airplay_features(dnssd, 27, pin_required); /* pairing with a code, as UxPlay's -pin */

    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.video_report_size = cb_video_report_size;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec = cb_video_set_codec;
    cbs.report_client_request = cb_report_client_request;
    if (pin_required) {
        cbs.display_pin = cb_display_pin;
        cbs.register_client = cb_register_client;
        cbs.check_register = cb_check_register;
    }
    cbs.on_video_play = cb_on_video_play;
    cbs.on_video_scrub = cb_on_video_scrub;
    cbs.on_video_rate = cb_on_video_rate;
    cbs.on_video_stop = cb_on_video_stop;
    cbs.on_video_acquire_playback_info = cb_on_video_acquire_playback_info;
    cbs.on_video_playlist_remove = cb_on_video_playlist_remove;

    say("step: raop_init");
    raop = raop_init(&cbs);
    if (!raop) {
        say("RESULT: raop_init failed");
        return 1;
    }
    the_raop = raop;
    hls_init(say);
    raop_set_log_callback(raop, cb_log, NULL);
    raop_set_log_level(raop, MIRAGE_LOG_LEVEL);

    say("step: raop_init2 (key pair)");
    if (raop_init2(raop, 1, device_id, key_file)) {
        say("RESULT: raop_init2 failed");
        return 1;
    }
    if (pin_required)
        raop_set_plist(raop, "pin", 0); /* 0: a new random code each time */
    if (MIRAGE_HLS) {
        raop_set_plist(raop, "hls", 1);
        raop_set_lang(raop, "en", "", "en");
    }
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);

    say("step: start the RTSP/HTTP listener");
    port = raop_get_port(raop);
    raop_start_httpd(raop, &port);
    raop_set_port(raop, port);
    say("  listening on TCP %u", port);
    raop_set_dnssd(raop, dnssd);

    say("step: advertise _raop._tcp and _airplay._tcp");
    error = dnssd_register_raop(dnssd, port);
    if (error)
        say("  dnssd_register_raop error %d", error);
    error = dnssd_register_airplay(dnssd, port);
    if (error)
        say("  dnssd_register_airplay error %d", error);
    say("  features 0x%llx", (unsigned long long)dnssd_get_airplay_features(dnssd));
    say("ready");

    for (int second = 1; MIRAGE_RUN_SECONDS == 0 || second <= MIRAGE_RUN_SECONDS; ++second) {
        sleep(1);
        /* Every 10 s while something is connected, every 5 minutes otherwise:
         * this runs for days and the log is 256 KiB. */
        if ((second % 10 == 0 && atomic_load(&connections) > 0) || second % 300 == 0)
            say("status: %d s, connections %d, audio %lu packets, %lu frames out, %lu decode errors, %lu invalid, "
                "title %s, %lu messages without a title, video %lu packets",
                second, atomic_load(&connections), atomic_load(&audio_packets), audio_pipeline_frames_out(),
                audio_pipeline_errors(), audio_pipeline_invalid(),
                pcm_server_connected() ? "connected" : "not connected",
                pcm_server_dropped(), atomic_load(&video_packets));
        if (atomic_load(&pin_shown_at) != 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((long)now.tv_sec - atomic_load(&pin_shown_at) > PIN_SHOWN_SECONDS) {
                say("pairing: nobody entered the code in %d s; taking it off the screen", PIN_SHOWN_SECONDS);
                set_pin("");
            }
        }
        if (pcm_server_quit_requested()) {
            say("stopping: a newer copy of the payload asked for the place");
            break;
        }
    }

    say("step: stop advertising");
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    say("step: stop the listener");
    raop_stop_httpd(raop);
    say("step: destroy");
    raop_destroy(raop);
    dnssd_destroy(dnssd);
    audio_pipeline_destroy();
    /* The log port first: the link port is what a waiting newer copy binds,
     * and it opens its own log port right after. */
    log_server_stop();
    pcm_server_stop();
    say("RESULT: mirage finished");
    return 0;
}
