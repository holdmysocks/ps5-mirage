/*
 * Mirage - AirPlay audio packets to 48 kHz stereo PCM on the link.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MIRAGE_AUDIO_PIPELINE_H
#define MIRAGE_AUDIO_PIPELINE_H

#include <stdint.h>

#include "pcm_server.h"

int audio_pipeline_init(pcm_server_log_t log);
void audio_pipeline_destroy(void);

/* One decrypted AirPlay audio packet. ct is the AirPlay compression type:
 * 2 ALAC (audio only), 8 AAC-ELD (the sound of screen mirroring).
 * time_ns is the packet's place on the stream's timeline; live says that
 * timeline is the sender's wall clock (pcm_link.h, PCM_LINK_FLAG_LIVE). */
void audio_pipeline_packet(unsigned char ct, const unsigned char *data, int length, uint64_t time_ns, int live);
void audio_pipeline_flush(void);
/* AirPlay volume in dB: 0 is full, -30 the lowest step, -144 mute. */
void audio_pipeline_set_volume(float db);

/* The volume as a gain, 32768 for full: for sound that does not go through
 * this pipeline (a cast's). */
int audio_pipeline_gain_q15(void);

unsigned long audio_pipeline_frames_out(void);
unsigned long audio_pipeline_errors(void);
/* Packets skipped because they did not start like a frame of their format. */
unsigned long audio_pipeline_invalid(void);

#endif
