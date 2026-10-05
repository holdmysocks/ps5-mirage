/*
 * Mirage - video casting (AirPlay "play this URL", as the YouTube app does).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * On a cast the module fetches the master playlist UxPlay serves, picks one
 * H.264 variant that the title's decoder can take (at most 1080 lines) and
 * its audio rendition, and opens both with FFmpeg's HLS demuxer (the segments
 * come from the content servers over HTTPS). A player thread then sends the
 * film to the title over the link, a little ahead of time and in step with
 * the clock: the video as it is (start-code form, in decoding order, flagged
 * as a film), the sound decoded and resampled to 48 kHz. Both carry the
 * film's own times, so the title shows each picture with its sound. Pause,
 * seek and stop from the app act on that thread.
 */
#include "hls.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "audio_pipeline.h"
#include "pcm_link.h"

#define MAX_PLAYLIST_BYTES (512 * 1024)
#define MAX_HEIGHT 1080
/* How far ahead of its time a packet is sent. The title holds it until it is
 * due; a segment download slower than this is heard. */
#define PLAY_LEAD_SECONDS 1.0
/* How much of the picture is sent ahead after a seek in a paused film. */
#define PREVIEW_SECONDS 0.3

static pcm_server_log_t say;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* Guarded by lock. */
static int active;
static int paused;
static double position_at_mark; /* seconds of the stream at the mark */
static struct timespec mark;    /* when position_at_mark was true */
static double duration_seconds; /* 0 until known */
static int playing;             /* the player thread is sending the film */
static int finished;            /* the film ran out, or could not be played */
static double seek_request = -1.0; /* seconds, -1 for none; taken by the player thread */
/* The YouTube app pauses the film when a drag of its position bar begins,
 * and after the seek says "pause" again and leaves it there. A film that was
 * playing until just before a seek therefore goes on playing after it. */
#define DRAG_PAUSE_SECONDS 10.0   /* a pause this soon before a seek is taken to be the drag's */
#define DRAG_ECHO_SECONDS 1.5     /* a pause this soon after such a seek is ignored */
static struct timespec paused_at;  /* when the film went from playing to paused */
static struct timespec resumed_by_seek_at;
static int resumed_by_seek;
static struct timespec play_asked; /* when the app asked for this film, for the log */
/* Bumped whenever the current cast ends or is replaced: a reader thread
 * that sees another value than the one it started with stops. */
static atomic_ulong generation;

static double elapsed_since(const struct timespec *from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - from->tv_sec) + (double)(now.tv_nsec - from->tv_nsec) / 1e9;
}

static double position_locked(void)
{
    return paused ? position_at_mark : position_at_mark + elapsed_since(&mark);
}

void hls_init(pcm_server_log_t log)
{
    say = log;
    avformat_network_init();
}

/* ---- the local playlist -------------------------------------------------- */

/* GET of an http:// URL on this machine. Returns the body (to be freed,
 * always terminated) or NULL. Never blocks for long. */
static char *fetch_local(const char *url, size_t *body_length)
{
    char host[64] = "", path[900] = "/", request[1100];
    struct sockaddr_in addr;
    char *reply, *body;
    int port = 80, fd, length;
    size_t got = 0;
    const char *p = url;

    if (strncmp(p, "http://", 7) != 0)
        return NULL;
    p += 7;
    {
        const char *slash = strchr(p, '/');
        const char *end = slash ? slash : p + strlen(p);
        const char *colon = memchr(p, ':', (size_t)(end - p));
        size_t host_length = (size_t)((colon ? colon : end) - p);
        if (host_length >= sizeof(host))
            return NULL;
        memcpy(host, p, host_length);
        host[host_length] = '\0';
        if (colon)
            port = atoi(colon + 1);
        if (slash)
            snprintf(path, sizeof(path), "%s", slash);
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (strcmp(host, "localhost") == 0)
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
        return NULL;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    reply = malloc(MAX_PLAYLIST_BYTES + 1);
    length = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path,
                      host, port);
    if (reply && length > 0 && send(fd, request, (size_t)length, 0) == length) {
        for (int waited = 0; waited < 5000 && got < MAX_PLAYLIST_BYTES; waited += 10) {
            ssize_t n = recv(fd, reply + got, MAX_PLAYLIST_BYTES - got, MSG_DONTWAIT);
            if (n == 0)
                break;
            if (n > 0) {
                /* The server keeps the connection open: stop at the length it gave. */
                const char *end, *field;
                got += (size_t)n;
                reply[got] = '\0';
                end = strstr(reply, "\r\n\r\n");
                field = end ? strcasestr(reply, "\r\nContent-Length:") : NULL;
                if (field && field < end && got >= (size_t)(end + 4 - reply) + (size_t)strtoul(field + 17, NULL, 10))
                    break;
            } else {
                usleep(10000);
            }
        }
    }
    close(fd);
    if (!reply)
        return NULL;
    reply[got] = '\0';
    body = strstr(reply, "\r\n\r\n");
    if (!body || strncmp(reply, "HTTP/1.1 200", 12) != 0) {
        free(reply);
        return NULL;
    }
    body += 4;
    *body_length = got - (size_t)(body - reply);
    memmove(reply, body, *body_length + 1);
    return reply;
}

/* The value of KEY= in an attribute list, unquoted, into out. */
static int attribute(const char *line, const char *key, char *out, size_t size)
{
    const size_t key_length = strlen(key);
    const char *p = line;

    out[0] = '\0';
    while ((p = strstr(p, key)) != NULL) {
        const int at_start = p == line || p[-1] == ',' || p[-1] == ':';
        if (at_start && p[key_length] == '=') {
            const char *value = p + key_length + 1;
            const char *end;
            if (*value == '"') {
                ++value;
                end = strchr(value, '"');
            } else {
                end = value + strcspn(value, ",\r\n");
            }
            if (!end)
                return 0;
            if ((size_t)(end - value) >= size)
                return 0;
            memcpy(out, value, (size_t)(end - value));
            out[end - value] = '\0';
            return 1;
        }
        p += key_length;
    }
    return 0;
}

/* FFmpeg resolves names through the SDK's resolver, which does not know
 * "localhost"; the playlists UxPlay serves are full of it. */
static void use_loopback_address(char *url, size_t size)
{
    static const char from[] = "http://localhost";
    if (strncmp(url, from, sizeof(from) - 1) == 0) {
        char rest[2048];
        snprintf(rest, sizeof(rest), "%s", url + sizeof(from) - 1);
        snprintf(url, size, "http://127.0.0.1%s", rest);
    }
}

typedef struct {
    char video_url[2048];
    char audio_url[2048];
    int width, height;
    long bandwidth;
    char codecs[96];
} choice_t;

/* Picks the variant to play: H.264 (the title's decoder is set up for it),
 * at most MAX_HEIGHT lines, the tallest of those and then the highest
 * bandwidth; and the audio rendition that variant names. */
static int choose_variant(char *master, choice_t *choice)
{
    char audio_group[64] = "", line_group[64], value[2048];
    char pending[1024] = "";
    int variants = 0;

    memset(choice, 0, sizeof(*choice));
    for (char *line = master; line && *line;) {
        char *next = strchr(line, '\n');
        size_t length = next ? (size_t)(next - line) : strlen(line);
        char saved = line[length];
        line[length] = '\0';
        if (length > 0 && line[length - 1] == '\r')
            line[length - 1] = '\0';

        if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
            snprintf(pending, sizeof(pending), "%s", line + 18);
        } else if (line[0] != '#' && line[0] != '\0' && pending[0] != '\0') {
            char codecs[96] = "", resolution[32] = "", bandwidth[32] = "", group[64] = "";
            int width = 0, height = 0;
            long bits;
            attribute(pending, "CODECS", codecs, sizeof(codecs));
            attribute(pending, "RESOLUTION", resolution, sizeof(resolution));
            attribute(pending, "BANDWIDTH", bandwidth, sizeof(bandwidth));
            attribute(pending, "AUDIO", group, sizeof(group));
            sscanf(resolution, "%dx%d", &width, &height);
            bits = atol(bandwidth);
            ++variants;
            say("cast: variant %d: %dx%d, %ld kbit/s, codecs %s, audio group %s", variants, width, height, bits / 1000,
                codecs, group);
            if (strstr(codecs, "avc1") && height > 0 && height <= MAX_HEIGHT && width <= 1920 &&
                (height > choice->height || (height == choice->height && bits > choice->bandwidth))) {
                snprintf(choice->video_url, sizeof(choice->video_url), "%s", line);
                snprintf(choice->codecs, sizeof(choice->codecs), "%s", codecs);
                snprintf(audio_group, sizeof(audio_group), "%s", group);
                choice->width = width;
                choice->height = height;
                choice->bandwidth = bits;
            }
            pending[0] = '\0';
        }
        line[length] = saved;
        line = next ? next + 1 : NULL;
    }
    if (choice->video_url[0] == '\0')
        return 0;

    /* The audio rendition of the chosen variant's group. */
    for (char *line = master; line && *line;) {
        char *next = strchr(line, '\n');
        size_t length = next ? (size_t)(next - line) : strlen(line);
        char saved = line[length];
        line[length] = '\0';
        if (strncmp(line, "#EXT-X-MEDIA:", 13) == 0 && strstr(line, "TYPE=AUDIO") &&
            attribute(line + 13, "GROUP-ID", line_group, sizeof(line_group)) && strcmp(line_group, audio_group) == 0 &&
            attribute(line + 13, "URI", value, sizeof(value)) && choice->audio_url[0] == '\0')
            snprintf(choice->audio_url, sizeof(choice->audio_url), "%s", value);
        line[length] = saved;
        line = next ? next + 1 : NULL;
    }
    use_loopback_address(choice->video_url, sizeof(choice->video_url));
    use_loopback_address(choice->audio_url, sizeof(choice->audio_url));
    return 1;
}

/* ---- reading the streams ------------------------------------------------- */

typedef struct {
    unsigned long generation;
    time_t deadline; /* an open or a read that is still going at this time is given up */
} reader_t;

static int interrupted(void *opaque)
{
    const reader_t *reader = opaque;
    return atomic_load(&generation) != reader->generation || time(NULL) > reader->deadline;
}

static AVFormatContext *open_stream(const char *what, const char *url, reader_t *reader)
{
    AVFormatContext *context = avformat_alloc_context();
    char error[128];
    int rc;

    if (!context)
        return NULL;
    context->interrupt_callback.callback = interrupted;
    context->interrupt_callback.opaque = reader;
    reader->deadline = time(NULL) + 20;
    rc = avformat_open_input(&context, url, NULL, NULL);
    if (rc < 0) {
        av_strerror(rc, error, sizeof(error));
        say("cast: %s: open failed: %s", what, error);
        return NULL;
    }
    reader->deadline = time(NULL) + 20;
    rc = avformat_find_stream_info(context, NULL);
    if (rc < 0) {
        av_strerror(rc, error, sizeof(error));
        say("cast: %s: no stream information: %s", what, error);
        avformat_close_input(&context);
        return NULL;
    }
    say("cast: %s: format %s, %u stream(s), duration %.1f s, at %.0f ms", what,
        context->iformat ? context->iformat->name : "?", context->nb_streams,
        context->duration > 0 ? (double)context->duration / AV_TIME_BASE : 0.0, elapsed_since(&play_asked) * 1e3);
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        const AVStream *stream = context->streams[i];
        const AVCodecParameters *p = stream->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO)
            say("cast: %s: stream %u video %s, profile %d level %d, %dx%d, %.2f fps, reorder depth %d, extradata %d bytes",
                what, i, avcodec_get_name(p->codec_id), p->profile, p->level, p->width, p->height,
                av_q2d(stream->avg_frame_rate), p->video_delay, p->extradata_size);
        else if (p->codec_type == AVMEDIA_TYPE_AUDIO)
            say("cast: %s: stream %u audio %s, profile %d, %d Hz, %d channels", what, i, avcodec_get_name(p->codec_id),
                p->profile, p->sample_rate, p->ch_layout.nb_channels);
        else
            say("cast: %s: stream %u %s", what, i, av_get_media_type_string(p->codec_type));
    }
    return context;
}

/* ---- playing ------------------------------------------------------------ */

/* Where the film's own times sit on the link's timeline, in nanoseconds: far
 * from zero, so that nothing is ever negative. */
#define TIMELINE_ORIGIN_NS 1000000000000LL

typedef struct {
    AVFormatContext *format;
    AVPacket *packet; /* the next packet of this input, if loaded */
    int loaded, ended, stream;
} input_t;

typedef struct {
    reader_t reader;
    input_t video, audio;
    AVBSFContext *annexb;     /* MP4-style H.264 units to start-code form, with SPS/PPS at keyframes */
    AVCodecContext *decoder;  /* the sound */
    SwrContext *resampler;
    AVFrame *frame;
    AVPacket *filtered;
    int16_t pcm[8192 * PCM_LINK_CHANNELS];
    unsigned long video_units, audio_chunks, send_failures;
} player_t;

static double packet_seconds(const input_t *input, int64_t stamp)
{
    return stamp == AV_NOPTS_VALUE ? -1.0 : (double)stamp * av_q2d(input->format->streams[input->stream]->time_base);
}

static int64_t timeline_ns(double seconds)
{
    return TIMELINE_ORIGIN_NS + (int64_t)(seconds * 1e9);
}

/* Loads the input's next packet of its one interesting stream. */
static void load(player_t *player, input_t *input)
{
    while (!input->loaded && !input->ended) {
        player->reader.deadline = time(NULL) + 30;
        if (av_read_frame(input->format, input->packet) < 0) {
            input->ended = 1;
        } else if (input->packet->stream_index == input->stream) {
            input->loaded = 1;
        } else {
            av_packet_unref(input->packet);
        }
    }
}

static int open_sound(player_t *player)
{
    const AVCodecParameters *parameters = player->audio.format->streams[player->audio.stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);

    if (!codec)
        return -1;
    player->decoder = avcodec_alloc_context3(codec);
    if (!player->decoder || avcodec_parameters_to_context(player->decoder, parameters) < 0)
        return -1;
    player->decoder->thread_count = 1;
    return avcodec_open2(player->decoder, codec, NULL) < 0 ? -1 : 0;
}

static int open_annexb(player_t *player)
{
    const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
    const AVStream *stream = player->video.format->streams[player->video.stream];

    if (!filter || av_bsf_alloc(filter, &player->annexb) < 0)
        return -1;
    if (avcodec_parameters_copy(player->annexb->par_in, stream->codecpar) < 0)
        return -1;
    player->annexb->time_base_in = stream->time_base;
    return av_bsf_init(player->annexb) < 0 ? -1 : 0;
}

static void send_video(player_t *player)
{
    const double seconds = packet_seconds(&player->video, player->video.packet->pts);

    if (av_bsf_send_packet(player->annexb, player->video.packet) < 0) {
        av_packet_unref(player->video.packet);
        return;
    }
    while (av_bsf_receive_packet(player->annexb, player->filtered) == 0) {
        if (pcm_server_send_film_video(timeline_ns(seconds), player->filtered->data, (uint32_t)player->filtered->size) == 0)
            ++player->video_units;
        else
            ++player->send_failures;
        av_packet_unref(player->filtered);
    }
}

static void send_sound(player_t *player)
{
    double seconds = packet_seconds(&player->audio, player->audio.packet->pts);

    if (avcodec_send_packet(player->decoder, player->audio.packet) < 0) {
        av_packet_unref(player->audio.packet);
        return;
    }
    av_packet_unref(player->audio.packet);
    while (avcodec_receive_frame(player->decoder, player->frame) == 0) {
        uint8_t *out[1] = {(uint8_t *)player->pcm};
        int got;
        if (!player->resampler) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&player->resampler, &stereo, AV_SAMPLE_FMT_S16, PCM_LINK_SAMPLE_RATE,
                                    &player->frame->ch_layout, (enum AVSampleFormat)player->frame->format,
                                    player->frame->sample_rate, 0, NULL) < 0 ||
                swr_init(player->resampler) < 0) {
                swr_free(&player->resampler);
                av_frame_unref(player->frame);
                return;
            }
        }
        got = swr_convert(player->resampler, out, 8192, (const uint8_t **)player->frame->extended_data,
                          player->frame->nb_samples);
        if (got > 0) {
            /* The sending device's volume, as for other AirPlay sound. */
            const int gain = audio_pipeline_gain_q15();
            if (gain != 32768)
                for (int i = 0; i < got * PCM_LINK_CHANNELS; ++i)
                    player->pcm[i] = (int16_t)(((int32_t)player->pcm[i] * gain) >> 15);
            pcm_server_send_audio(timeline_ns(seconds), 0, player->pcm,
                                  (uint32_t)got * PCM_LINK_CHANNELS * sizeof(int16_t));
            ++player->audio_chunks;
            seconds += (double)player->frame->nb_samples / (double)player->frame->sample_rate;
        }
        av_frame_unref(player->frame);
    }
}

/* Moves both inputs to a place in the film and makes the title forget what
 * it holds. */
static void seek_to(player_t *player, double seconds)
{
    const int64_t target = (int64_t)(seconds * AV_TIME_BASE);

    player->reader.deadline = time(NULL) + 30;
    av_seek_frame(player->video.format, -1, target, AVSEEK_FLAG_BACKWARD);
    if (player->audio.format != player->video.format)
        av_seek_frame(player->audio.format, -1, target, AVSEEK_FLAG_BACKWARD);
    for (input_t *input = &player->video;; input = &player->audio) {
        if (input->loaded)
            av_packet_unref(input->packet);
        input->loaded = 0;
        input->ended = 0;
        if (input == &player->audio)
            break;
    }
    if (player->decoder)
        avcodec_flush_buffers(player->decoder);
    if (player->annexb)
        av_bsf_flush(player->annexb);
    swr_free(&player->resampler);
    pcm_server_send(PCM_LINK_VIDEO_RESET);
    pcm_server_send(PCM_LINK_FLUSH);
}

/* After a seek the video can only start at a keyframe, and the demuxer goes
 * to the one before the place asked for (they are seconds apart). Playing
 * from the place asked for would mean running the decoder through that
 * stretch first while the sound is already going. So the film simply starts
 * at the keyframe: this returns its time, having dropped the sound that
 * comes before it. */
static double align_to_keyframe(player_t *player, double asked)
{
    double start = asked;

    load(player, &player->video);
    if (player->video.loaded) {
        const double first = packet_seconds(&player->video, player->video.packet->pts);
        if (first >= 0.0)
            start = first;
    }
    for (;;) {
        load(player, &player->audio);
        if (!player->audio.loaded || packet_seconds(&player->audio, player->audio.packet->pts) >= start - 0.03)
            break;
        av_packet_unref(player->audio.packet);
        player->audio.loaded = 0;
    }
    if (start != asked)
        say("cast: starting at the keyframe at %.2f s (asked for %.2f s)", start, asked);
    return start;
}

static double monotonic_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* The loop that plays: packets go to the title PLAY_LEAD_SECONDS before
 * their time, in order of time, against the wall clock. The title keeps what
 * it gets until it is due, so a slow segment download costs nothing as long
 * as it is shorter than the lead. */
static void play_loop(player_t *player, double start_seconds)
{
    double film_at_mark = start_seconds; /* film time at clock_mark */
    double clock_mark = monotonic_now();
    double last_report = clock_mark;
    int was_paused = 0, preview = 0;

    while (atomic_load(&generation) == player->reader.generation) {
        double want_seek = -1.0, film_now, next_time;
        input_t *next;
        int is_paused;

        pthread_mutex_lock(&lock);
        is_paused = paused;
        if (seek_request >= 0.0) {
            want_seek = seek_request;
            seek_request = -1.0;
        }
        pthread_mutex_unlock(&lock);

        if (want_seek >= 0.0) {
            say("cast: seeking to %.1f s", want_seek);
            seek_to(player, want_seek);
            film_at_mark = align_to_keyframe(player, want_seek);
            clock_mark = monotonic_now();
            if (was_paused) {
                pcm_server_send_hold(1); /* the seek cleared the title; it is still a paused film */
                preview = 1;
            }
        }
        if (is_paused) {
            if (!was_paused) { /* freeze the film clock where it is, and the title with it */
                film_at_mark += monotonic_now() - clock_mark;
                pcm_server_send_hold(1);
                was_paused = 1;
            }
            clock_mark = monotonic_now();
            if (preview) {
                /* A seek in a paused film: the first pictures of the new place
                 * go to the title now, so that it shows where the film is
                 * instead of nothing. The sound waits for the film to run. */
                load(player, &player->video);
                if (player->video.loaded &&
                    packet_seconds(&player->video, player->video.packet->dts) <= film_at_mark + PREVIEW_SECONDS) {
                    send_video(player);
                    player->video.loaded = 0;
                    continue;
                }
                preview = 0;
            }
            usleep(20000);
            continue;
        }
        preview = 0;
        if (was_paused) {
            pcm_server_send_hold(0);
            was_paused = 0;
        }

        load(player, &player->video);
        load(player, &player->audio);
        if (!player->video.loaded && !player->audio.loaded)
            break; /* both ended (or the cast did) */
        {
            const double video_time =
                player->video.loaded ? packet_seconds(&player->video, player->video.packet->dts) : 1e18;
            const double audio_time =
                player->audio.loaded ? packet_seconds(&player->audio, player->audio.packet->pts) : 1e18;
            next = video_time <= audio_time ? &player->video : &player->audio;
            next_time = video_time <= audio_time ? video_time : audio_time;
        }
        film_now = film_at_mark + (monotonic_now() - clock_mark);
        if (next_time > film_now + PLAY_LEAD_SECONDS) {
            usleep(5000); /* not yet */
        } else {
            /* A seek lands on the keyframe before the place asked for:
             * everything from there is sent, the title shows it as it comes. */
            if (next == &player->video)
                send_video(player);
            else
                send_sound(player);
            next->loaded = 0;
        }

        pthread_mutex_lock(&lock);
        if (atomic_load(&generation) == player->reader.generation && !paused && seek_request < 0.0) {
            position_at_mark = film_now;
            clock_gettime(CLOCK_MONOTONIC, &mark);
        }
        pthread_mutex_unlock(&lock);
        if (monotonic_now() - last_report >= 10.0) {
            last_report = monotonic_now();
            say("cast: at %.1f s, %lu video units and %lu sound chunks sent, %lu not taken, title %s", film_now,
                player->video_units, player->audio_chunks, player->send_failures,
                pcm_server_connected() ? "connected" : "not connected");
        }
    }
}

typedef struct {
    char url[1024];
    unsigned long generation;
    double start_seconds;
} play_request_t;

static void *play_main(void *arg)
{
    play_request_t *request = arg;
    player_t *player = calloc(1, sizeof(*player));
    choice_t choice;
    size_t length = 0;
    char *master = fetch_local(request->url, &length);
    int ready = 0;

    if (!player || !master) {
        say("cast: could not fetch the master playlist from %s", request->url);
        goto done;
    }
    player->reader.generation = request->generation;
    say("cast: master playlist, %zu bytes, at %.0f ms", length, elapsed_since(&play_asked) * 1e3);
    if (!choose_variant(master, &choice)) {
        say("cast: no H.264 variant of at most %d lines in the playlist; nothing to play", MAX_HEIGHT);
        goto done;
    }
    say("cast: chosen %dx%d, %ld kbit/s, %s", choice.width, choice.height, choice.bandwidth / 1000, choice.codecs);

    player->video.format = open_stream("video", choice.video_url, &player->reader);
    if (player->video.format && choice.audio_url[0] && !interrupted(&player->reader))
        player->audio.format = open_stream("audio", choice.audio_url, &player->reader);
    if (!player->video.format || !player->audio.format) {
        say("cast: the streams did not open; nothing to play");
        goto done;
    }
    player->video.stream = av_find_best_stream(player->video.format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    player->audio.stream = av_find_best_stream(player->audio.format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    player->video.packet = av_packet_alloc();
    player->audio.packet = av_packet_alloc();
    player->filtered = av_packet_alloc();
    player->frame = av_frame_alloc();
    if (player->video.stream < 0 || player->audio.stream < 0 || !player->video.packet || !player->audio.packet ||
        !player->filtered || !player->frame || open_annexb(player) != 0 || open_sound(player) != 0) {
        say("cast: could not set up the video filter or the sound decoder");
        goto done;
    }

    pthread_mutex_lock(&lock);
    if (atomic_load(&generation) == request->generation && player->video.format->duration > 0)
        duration_seconds = (double)player->video.format->duration / AV_TIME_BASE;
    pthread_mutex_unlock(&lock);

    /* Nothing can be shown without a title; the cast itself has asked for one. */
    for (int waited = 0; waited < 200 && !pcm_server_connected() && !interrupted(&player->reader); ++waited) {
        player->reader.deadline = time(NULL) + 30;
        usleep(100000);
    }
    if (!pcm_server_connected()) {
        say("cast: no title connected in 20 s; not playing");
        goto done;
    }
    {
        const AVCodecParameters *p = player->video.format->streams[player->video.stream]->codecpar;
        pcm_server_send_video_size((uint32_t)p->width, (uint32_t)p->height, (uint32_t)p->width, (uint32_t)p->height);
    }
    seek_to(player, request->start_seconds); /* also clears the title */
    ready = 1;
    pthread_mutex_lock(&lock);
    if (atomic_load(&generation) == request->generation)
        playing = 1;
    pthread_mutex_unlock(&lock);
    {
        const double start = align_to_keyframe(player, request->start_seconds);
        say("cast: playing from %.1f s, %.0f ms after the app asked", start, elapsed_since(&play_asked) * 1e3);
        play_loop(player, start);
    }

    if (atomic_load(&generation) == request->generation) {
        /* The film ran out: let what the title holds play, then say so. */
        say("cast: end of the film after %lu video units and %lu sound chunks", player->video_units,
            player->audio_chunks);
        usleep((useconds_t)((PLAY_LEAD_SECONDS + 0.5) * 1e6));
        pthread_mutex_lock(&lock);
        if (atomic_load(&generation) == request->generation)
            finished = 1;
        pthread_mutex_unlock(&lock);
    } else {
        say("cast: stopped after %lu video units and %lu sound chunks (%lu not taken)", player->video_units,
            player->audio_chunks, player->send_failures);
    }

done:
    if (!ready && player && atomic_load(&generation) == request->generation) {
        pthread_mutex_lock(&lock);
        finished = 1; /* the app is told the video is over rather than left waiting */
        pthread_mutex_unlock(&lock);
    }
    if (player) {
        if (ready) {
            pcm_server_send(PCM_LINK_VIDEO_RESET);
            pcm_server_send(PCM_LINK_FLUSH);
        }
        av_bsf_free(&player->annexb);
        avcodec_free_context(&player->decoder);
        swr_free(&player->resampler);
        av_frame_free(&player->frame);
        av_packet_free(&player->filtered);
        av_packet_free(&player->video.packet);
        av_packet_free(&player->audio.packet);
        if (player->audio.format)
            avformat_close_input(&player->audio.format);
        if (player->video.format)
            avformat_close_input(&player->video.format);
        free(player);
    }
    free(master);
    free(request);
    return NULL;
}

/* ---- UxPlay's callbacks -------------------------------------------------- */

void hls_on_play(const char *where, float start_position)
{
    play_request_t *request = calloc(1, sizeof(*request));
    pthread_attr_t attr;
    pthread_t thread;

    pthread_mutex_lock(&lock);
    active = 1;
    paused = 0;
    playing = 0;
    finished = 0;
    seek_request = -1.0;
    resumed_by_seek = 0;
    duration_seconds = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &play_asked);
    position_at_mark = start_position > 0.0f ? (double)start_position : 0.0;
    clock_gettime(CLOCK_MONOTONIC, &mark);
    pthread_mutex_unlock(&lock);
    say("cast: play %s from %.1f s", where ? where : "(null)", (double)start_position);
    if (!request)
        return;
    snprintf(request->url, sizeof(request->url), "%s", where ? where : "");
    request->start_seconds = start_position > 0.0f ? (double)start_position : 0.0;
    request->generation = atomic_fetch_add(&generation, 1) + 1;

    /* Off UxPlay's thread: it is the HTTP server that answers the fetches. */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, play_main, request) != 0) {
        say("cast: could not start the player thread");
        free(request);
    }
    pthread_attr_destroy(&attr);
}

void hls_on_scrub(float position)
{
    int resumed = 0;

    pthread_mutex_lock(&lock);
    seek_request = position >= 0.0f ? (double)position : 0.0;
    if (paused && elapsed_since(&paused_at) < DRAG_PAUSE_SECONDS) {
        paused = 0;
        resumed_by_seek = resumed = 1;
        clock_gettime(CLOCK_MONOTONIC, &resumed_by_seek_at);
    }    position_at_mark = seek_request;
    clock_gettime(CLOCK_MONOTONIC, &mark);
    pthread_mutex_unlock(&lock);
    say("cast: seek to %.1f s asked%s", (double)position, resumed ? "; the pause before it was the drag, playing on" : "");
}

void hls_on_rate(float rate)
{
    int ignored = 0;

    pthread_mutex_lock(&lock);
    if (rate == 0.0f && resumed_by_seek && elapsed_since(&resumed_by_seek_at) < DRAG_ECHO_SECONDS) {
        ignored = 1;
    } else {
        position_at_mark = position_locked();
        clock_gettime(CLOCK_MONOTONIC, &mark);
        if (rate == 0.0f && !paused)
            clock_gettime(CLOCK_MONOTONIC, &paused_at);
        paused = rate == 0.0f;
    }
    resumed_by_seek = 0;
    pthread_mutex_unlock(&lock);
    say("cast: rate %.2f (%s)%s", (double)rate, rate == 0.0f ? "pause" : "play",
        ignored ? ", ignored: it follows a drag seek" : "");
}

void hls_on_stop(void)
{
    say("cast: stop");
    hls_reset();
}

float hls_on_playlist_remove(void)
{
    float position;
    pthread_mutex_lock(&lock);
    position = (float)position_locked();
    pthread_mutex_unlock(&lock);
    say("cast: playlist removed at %.1f s", (double)position);
    return position;
}

void hls_on_playback_info(playback_info_t *info)
{
    pthread_mutex_lock(&lock);
    if (finished) {
        /* UxPlay's convention for "the video is over": the library then ends the cast. */
        info->position = -1.0;
        info->duration = -1.0;
    } else if (!active || !playing || duration_seconds <= 0.0) {
        /* Not playing yet: "no information", which the library passes on as such. */
        info->position = -1.0;
        info->duration = 0.0;
    } else {
        /* The player thread keeps the mark at the film's clock, which is what
         * is on screen give or take the title's small delay. */
        info->position = position_locked();
        info->duration = duration_seconds;
        if (info->position > info->duration)
            info->position = info->duration;
        if (info->position < 0.0)
            info->position = 0.0;
    }
    info->rate = active && playing && !paused ? 1.0f : 0.0f;
    pthread_mutex_unlock(&lock);
    info->seek_start = 0.0;
    info->seek_duration = info->duration > 0.0 ? info->duration : 0.0;
    info->ready_to_play = true;
    info->playback_buffer_empty = false;
    info->playback_buffer_full = true;
    info->playback_likely_to_keep_up = true;
}

void hls_reset(void)
{
    atomic_fetch_add(&generation, 1); /* the player thread stops at its next step */
    pthread_mutex_lock(&lock);
    active = 0;
    paused = 0;
    playing = 0;
    finished = 0;
    seek_request = -1.0;
    resumed_by_seek = 0;
    duration_seconds = 0.0;
    pthread_mutex_unlock(&lock);
}

int hls_active(void)
{
    int result;
    pthread_mutex_lock(&lock);
    result = active;
    pthread_mutex_unlock(&lock);
    return result;
}
