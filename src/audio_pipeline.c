/*
 * Mirage - AirPlay audio packets to 48 kHz stereo PCM on the link.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two formats arrive, both 44.1 kHz stereo, and both are decoded with FFmpeg
 * and resampled to the 48 kHz that AudioOut wants:
 *   ct 2  ALAC, 352 samples per packet: audio-only AirPlay
 *   ct 8  AAC-ELD, 480 samples per packet: the sound of screen mirroring
 * All entry points may be called from UxPlay's threads.
 */
#include "audio_pipeline.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include "pcm_link.h"

#define SOURCE_RATE 44100
#define MAX_OUT_FRAMES 4096

/* The ALAC "magic cookie" for AirPlay's fixed format, as UxPlay's GStreamer
 * renderer passes it: 352 frames per packet, 16 bit, stereo, 44100 Hz. */
static const uint8_t alac_cookie[36] = {
    0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};
/* The MPEG-4 AudioSpecificConfig for AAC-ELD, 44100 Hz, stereo, 480 samples
 * per frame, likewise as UxPlay passes it. */
static const uint8_t eld_config[4] = {0xf8, 0xe8, 0x50, 0x00};

enum { FORMAT_ALAC, FORMAT_ELD, FORMAT_COUNT, FORMAT_NONE = -1 };

static pcm_server_log_t say;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AVCodecContext *decoders[FORMAT_COUNT];
static AVPacket *packet;
static AVFrame *frame;
static SwrContext *resampler;
static int resampler_format = FORMAT_NONE; /* which decoder the resampler was opened for */
static int16_t out[MAX_OUT_FRAMES * PCM_LINK_CHANNELS];
static atomic_int gain_q15 = 32768;
static atomic_ulong frames_out, errors, invalid;
static int warned_ct, warned_invalid;

static AVCodecContext *open_decoder(enum AVCodecID id, const char *name, const uint8_t *extradata, int extradata_size)
{
    const AVCodec *codec = avcodec_find_decoder(id);
    AVCodecContext *context;

    if (!codec) {
        say("audio: no %s decoder in this FFmpeg", name);
        return NULL;
    }
    context = avcodec_alloc_context3(codec);
    if (!context)
        return NULL;
    context->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata) {
        avcodec_free_context(&context);
        return NULL;
    }
    memcpy(context->extradata, extradata, (size_t)extradata_size);
    context->extradata_size = extradata_size;
    context->sample_rate = SOURCE_RATE;
    av_channel_layout_default(&context->ch_layout, 2);
    context->thread_count = 1;
    if (avcodec_open2(context, codec, NULL) < 0) {
        say("audio: the %s decoder did not open", name);
        avcodec_free_context(&context);
        return NULL;
    }
    return context;
}

int audio_pipeline_init(pcm_server_log_t log)
{
    say = log;
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame)
        return -1;
    decoders[FORMAT_ALAC] = open_decoder(AV_CODEC_ID_ALAC, "ALAC", alac_cookie, sizeof(alac_cookie));
    decoders[FORMAT_ELD] = open_decoder(AV_CODEC_ID_AAC, "AAC-ELD", eld_config, sizeof(eld_config));
    if (!decoders[FORMAT_ALAC])
        return -1;
    /* Mirroring still shows a picture without its sound, so a missing
     * AAC-ELD decoder is reported, not fatal. */
    say("audio: ALAC decoder ready, AAC-ELD decoder %s (FFmpeg %s)", decoders[FORMAT_ELD] ? "ready" : "MISSING",
        av_version_info());
    return 0;
}

void audio_pipeline_destroy(void)
{
    pthread_mutex_lock(&lock);
    swr_free(&resampler);
    for (int i = 0; i < FORMAT_COUNT; ++i)
        avcodec_free_context(&decoders[i]);
    av_packet_free(&packet);
    av_frame_free(&frame);
    pthread_mutex_unlock(&lock);
}

/* Opened from the first decoded frame, whose format is the decoder's real
 * output (ALAC gives 16 bit planar, AAC float planar). */
static int open_resampler(int format)
{
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int rc;

    swr_free(&resampler);
    rc = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, PCM_LINK_SAMPLE_RATE, &frame->ch_layout,
                             (enum AVSampleFormat)frame->format, frame->sample_rate > 0 ? frame->sample_rate : SOURCE_RATE,
                             0, NULL);
    if (rc < 0 || swr_init(resampler) < 0) {
        swr_free(&resampler);
        resampler_format = FORMAT_NONE;
        return -1;
    }
    resampler_format = format;
    return 0;
}

static void apply_gain(int16_t *samples, int count)
{
    const int gain = atomic_load(&gain_q15);
    if (gain == 32768)
        return;
    for (int i = 0; i < count; ++i)
        samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 15);
}

static void emit(const uint8_t **in, int in_frames, int64_t time_ns, int live)
{
    uint8_t *out_planes[1] = {(uint8_t *)out};
    int got = swr_convert(resampler, out_planes, MAX_OUT_FRAMES, in, in_frames);
    if (got < 0) {
        atomic_fetch_add(&errors, 1);
        return;
    }
    if (got > 0) {
        apply_gain(out, got * PCM_LINK_CHANNELS);
        pcm_server_send_audio(time_ns, live, out, (uint32_t)got * PCM_LINK_CHANNELS * sizeof(int16_t));
        atomic_fetch_add(&frames_out, (unsigned long)got);
    }
}

/* The first byte tells a real frame from one that was not decrypted properly
 * (UxPlay's renderer makes the same check): ALAC frames start 0x20; AAC-ELD
 * 0x8c-0x8e on current devices, 0x80-0x82 on old ones. */
static int plausible(int format, const unsigned char *data)
{
    if (format == FORMAT_ALAC)
        return data[0] == 0x20;
    return (data[0] >= 0x8c && data[0] <= 0x8e) || (data[0] >= 0x80 && data[0] <= 0x82);
}

void audio_pipeline_packet(unsigned char ct, const unsigned char *data, int length, uint64_t time_ns, int live)
{
    const int format = ct == 2 ? FORMAT_ALAC : (ct == 8 ? FORMAT_ELD : FORMAT_NONE);
    AVCodecContext *decoder;

    if (format == FORMAT_NONE) {
        if (!warned_ct) {
            warned_ct = 1;
            say("audio: compression type %u is not handled, dropping", ct);
        }
        return;
    }
    if (length <= 0)
        return;
    if (!plausible(format, data)) {
        atomic_fetch_add(&invalid, 1);
        if (!warned_invalid) {
            warned_invalid = 1;
            say("audio: ct %u frame starts 0x%02x, not a valid frame; such frames are skipped", ct, data[0]);
        }
        return;
    }

    pthread_mutex_lock(&lock);
    decoder = decoders[format];
    if (!decoder || av_new_packet(packet, length) < 0) {
        pthread_mutex_unlock(&lock);
        return;
    }
    memcpy(packet->data, data, (size_t)length);
    if (avcodec_send_packet(decoder, packet) < 0) {
        atomic_fetch_add(&errors, 1);
    } else {
        while (avcodec_receive_frame(decoder, frame) == 0) {
            if ((!resampler || resampler_format != format) && open_resampler(format) != 0) {
                atomic_fetch_add(&errors, 1);
                av_frame_unref(frame);
                break;
            }
            /* The resampler holds back a few samples (well under a
             * millisecond); the packet's time is used as it is. */
            emit((const uint8_t **)frame->extended_data, frame->nb_samples, (int64_t)time_ns, live);
            av_frame_unref(frame);
        }
    }
    av_packet_unref(packet);
    pthread_mutex_unlock(&lock);
}

void audio_pipeline_flush(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < FORMAT_COUNT; ++i)
        if (decoders[i])
            avcodec_flush_buffers(decoders[i]);
    /* Reopened on the next packet, which also clears its filter history. */
    swr_free(&resampler);
    resampler_format = FORMAT_NONE;
    pthread_mutex_unlock(&lock);
    pcm_server_send(PCM_LINK_FLUSH);
}

void audio_pipeline_set_volume(float db)
{
    double gain = db <= -144.0f ? 0.0 : pow(10.0, (double)db / 20.0);
    if (gain > 1.0)
        gain = 1.0;
    atomic_store(&gain_q15, (int)(gain * 32768.0));
}

int audio_pipeline_gain_q15(void)
{
    return atomic_load(&gain_q15);
}

unsigned long audio_pipeline_frames_out(void)
{
    return atomic_load(&frames_out);
}

unsigned long audio_pipeline_errors(void)
{
    return atomic_load(&errors);
}

unsigned long audio_pipeline_invalid(void)
{
    return atomic_load(&invalid);
}
