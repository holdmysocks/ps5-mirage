/*
 * Mirage - the link between the payload (network, AirPlay protocol, decoding)
 * and the title (AudioOut, later video).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A loader payload cannot be heard and a title cannot listen on a socket
 * without a credential patch, so the payload listens on loopback and the
 * title connects to it. This header is shared by both sides.
 *
 * Both directions carry the same kind of message. Nearly all go from the
 * payload to the title; the title sends one, PCM_LINK_HELLO.
 *   byte 0-1  'M' 'P'
 *   byte 2    type (PCM_LINK_*)
 *   byte 3    flags (PCM_LINK_FLAG_*), 0 unless the type says otherwise
 *   byte 4-7  length of the body in bytes, little endian
 *   body
 *
 * The payload treats the end of the connection as "the title is gone".
 */
#ifndef MIRAGE_PCM_LINK_H
#define MIRAGE_PCM_LINK_H

#include <stdint.h>

#define PCM_LINK_PORT 47010
#define PCM_LINK_HEADER_SIZE 8
/* Large enough for a 4K keyframe; audio messages are a few kilobytes. */
#define PCM_LINK_MAX_BODY (4 * 1024 * 1024)

/* Body: an 8 byte little endian presentation time, then interleaved signed
 * 16 bit little endian stereo at 48 kHz.
 *
 * The time is that of the first frame, in nanoseconds on the stream's own
 * timeline, with an arbitrary origin: consecutive messages of one stream are
 * PCM_LINK_SAMPLE_RATE frames per second apart. It says when the audio is
 * due relative to the rest of the stream, not when to play it: the title
 * ties the timeline to its clock when the stream starts. */
#define PCM_LINK_AUDIO 1
#define PCM_LINK_AUDIO_PREFIX 8
/* Flag on an audio message: its time is the sender's wall clock and the
 * stream is live (the sound of screen mirroring). Such a stream keeps one
 * clock across pauses, so the title holds its delay steady by that clock
 * and skips sound that arrives too late. Without the flag (music) the
 * timeline may restart anywhere, the sender may send far ahead, and the
 * title plays everything from the first message on. */
#define PCM_LINK_FLAG_LIVE 1
/* No body. Drop everything buffered (seek, pause, track change); the next
 * audio message starts a new timeline. */
#define PCM_LINK_FLUSH 2
/* No body. The AirPlay session ended. */
#define PCM_LINK_STOP 3

/* Screen mirroring.
 *
 * PCM_LINK_VIDEO: one access unit. Body: PCM_LINK_VIDEO_PREFIX bytes, then
 * the unit in Annex-B form (start codes; SPS and PPS are in the stream ahead
 * of the pictures that need them).
 *   byte 0-7   time, little endian nanoseconds: UxPlay's ntp_time_remote, the
 *              sender's clock. The sound of a mirroring session carries the
 *              same clock in its audio messages.
 *   byte 8     codec: PCM_LINK_CODEC_H264 or PCM_LINK_CODEC_H265
 *   byte 9     number of NAL units as counted by UxPlay (255 if more)
 *   byte 10-11 0 */
#define PCM_LINK_VIDEO 4
#define PCM_LINK_VIDEO_PREFIX 12
/* Flag on a video message: the stream is a film, not a mirrored screen. Its
 * units come in decoding order and may show in another (B-frames), and they
 * are sent ahead of time. The title then must not force a picture out of the
 * decoder after every unit, must put pictures in order of their times, and
 * must hold units back until their pictures are nearly due. */
#define PCM_LINK_FLAG_ORDERED 2
#define PCM_LINK_CODEC_H264 0
#define PCM_LINK_CODEC_H265 1
/* Body: four little endian 32 bit numbers: source width, source height,
 * picture width, picture height, in pixels, as the sender reports them. */
#define PCM_LINK_VIDEO_SIZE 5
#define PCM_LINK_VIDEO_SIZE_BODY 16
/* No body. The video stream ended or restarts; drop decoder state. */
#define PCM_LINK_VIDEO_RESET 6

/* Title to payload, once, right after connecting. Body: the console's name
 * as the system reports it (UTF-8, no terminator, at most
 * PCM_LINK_HELLO_MAX bytes, possibly empty). The payload advertises itself
 * as "Mirage - <name>", so that several consoles can be told apart; a title
 * can ask the system for the name without loading anything new, a payload
 * cannot. */
#define PCM_LINK_HELLO 7
#define PCM_LINK_HELLO_MAX 64

/* Payload to title. Body: the name the receiver is advertised under ("Mirage
 * - <console>"), UTF-8, no terminator, at most PCM_LINK_NAME_MAX bytes. Sent
 * when a title connects and whenever the name is settled, for the title's
 * screen. */
#define PCM_LINK_NAME 9
#define PCM_LINK_NAME_MAX 96

/* Payload to title. Body: one byte, 1 to hold and 0 to go on. While held the
 * title plays silence and keeps the picture that is on screen; what it has
 * buffered stays, and on "go on" everything continues from where it stopped
 * (its schedule moves by the length of the hold). A cast's pause: the film
 * is sent a second ahead, and without this a pause would only be seen once
 * that second had played out. */
#define PCM_LINK_HOLD 10

/* Payload to title. Body: the pairing code a connecting device has to enter
 * (digits, no terminator, at most PCM_LINK_PIN_MAX bytes), for the title to
 * show; an empty body takes it off the screen again. Only with PIN pairing
 * switched on. */
#define PCM_LINK_PIN 11
#define PCM_LINK_PIN_MAX 8

/* To the payload, no body: stop. Sent by a newly started copy of the payload
 * that found the link port taken, so that sending the ELF again replaces the
 * running one (an update, or a restart). */
#define PCM_LINK_QUIT 8

#define PCM_LINK_SAMPLE_RATE 48000
#define PCM_LINK_CHANNELS 2

static inline void pcm_link_header(uint8_t header[PCM_LINK_HEADER_SIZE], uint8_t type, uint8_t flags, uint32_t length)
{
    header[0] = 'M';
    header[1] = 'P';
    header[2] = type;
    header[3] = flags;
    header[4] = (uint8_t)length;
    header[5] = (uint8_t)(length >> 8);
    header[6] = (uint8_t)(length >> 16);
    header[7] = (uint8_t)(length >> 24);
}

static inline void pcm_link_put_time(uint8_t prefix[PCM_LINK_AUDIO_PREFIX], int64_t time_ns)
{
    for (int i = 0; i < 8; ++i)
        prefix[i] = (uint8_t)((uint64_t)time_ns >> (8 * i));
}

static inline int64_t pcm_link_get_time(const uint8_t prefix[PCM_LINK_AUDIO_PREFIX])
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= (uint64_t)prefix[i] << (8 * i);
    return (int64_t)value;
}

#endif
