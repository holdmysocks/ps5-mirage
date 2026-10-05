/*
 * Mirage - payload side of the PCM link.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MIRAGE_PCM_SERVER_H
#define MIRAGE_PCM_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef void (*pcm_server_log_t)(const char *fmt, ...);

/* Listens on 127.0.0.1:PCM_LINK_PORT and serves one title at a time. If the
 * port is taken, another copy of the payload is running: it is asked to stop
 * (PCM_LINK_QUIT) and the port is tried again for a few seconds. Fails if
 * the port still cannot be had. */
int pcm_server_start(pcm_server_log_t log);
/* The name the receiver is advertised under; told to every title (PCM_LINK_NAME). */
void pcm_server_set_name(const char *name);
/* The pairing code to show ("" for none); told to the title at once and to
 * every title that connects while it stands (PCM_LINK_PIN). */
void pcm_server_set_pin(const char *pin);
/* Whether a newer copy has asked this one to stop. */
int pcm_server_quit_requested(void);
void pcm_server_stop(void);

/* Each sends one message to the title if one is connected, otherwise drops
 * it. Never blocks for long: a title that stops reading is disconnected. */
void pcm_server_send(uint8_t type);
/* Holds (1) or releases (0) the title's playback: PCM_LINK_HOLD. */
void pcm_server_send_hold(int hold);
void pcm_server_send_audio(int64_t time_ns, int live, const int16_t *samples, uint32_t sample_bytes);
void pcm_server_send_video(int64_t time_ns, int h265, int nal_count, const uint8_t *unit, uint32_t unit_bytes);
/* The same for a film (PCM_LINK_FLAG_ORDERED). Never kept for a later title:
 * the sender waits for a title before it starts. Returns 0 if it was sent. */
int pcm_server_send_film_video(int64_t time_ns, const uint8_t *unit, uint32_t unit_bytes);
void pcm_server_send_video_size(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height);

/* The console name a title has sent (PCM_LINK_HELLO). Returns 1 and fills
 * name (always terminated, possibly empty) once one has arrived, else 0. */
int pcm_server_hello(char *name, size_t size);

int pcm_server_connected(void);
/* Whether a title has connected at least once since the start. */
int pcm_server_ever_connected(void);
/* Seconds since a title was last connected (0 while one is), or since the
 * start if none has connected yet. */
unsigned pcm_server_seconds_without_title(void);
unsigned long pcm_server_dropped(void);

#endif
