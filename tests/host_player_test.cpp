/*
 * Mirage - host test of the title's player (title\src\player.cpp).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Builds the real player on Linux with AudioOut replaced by a stub that paces
 * like the console (one 256 frame grain per call) and counts what it is
 * given. A fake payload serves a tone on the link port with the arrival
 * pattern named on the command line. Run by tools\test-player.ps1.
 *
 *   steady   packets arrive exactly on time
 *   burst    the first second arrives at once, then on time
 *   stall    on time, with one 100 ms stall and catch-up after 2 s: inside
 *            the 150 ms start delay, so it must not be heard
 *   gap      on time, with one 400 ms stall and catch-up after 2 s: longer
 *            than the delay, so playback runs dry once and must recover
 *   flush    on time, a flush and a timeline jump after 2 s
 *   frozen   every message carries the same time (the bug of 2026-10-05:
 *            the player must not be silent for ever; it restarts each time)
 *   hold     a cast paused for a second after 2 s: a hold message, nothing
 *            sent meanwhile, a release, and the stream goes on where it
 *            stopped. Nothing may be lost or skipped, and no sound may play
 *            during the hold
 *   resume   a live stream (wall-clock times): steady, then a pause of one
 *            second with no messages, then the sound comes back with its
 *            first 300 ms arriving late and all at once. The delay must be
 *            back to normal soon after, not stuck 300 ms long (the fault
 *            seen on the console, 2026-10-05); the late part is skipped
 *   rotate   as video, but the parameter set changes after 2 s and back after
 *            4 s (the phone is turned and turned back), and one decode call
 *            fails for no reason: the decoder must be reset for each change,
 *            recover from the failure, and keep producing pictures
 *   late     as video, but the title is late: the first five seconds of
 *            video (150 units, starting at the stream's only keyframe) arrive
 *            all at once, as the payload hands over what it kept while the
 *            title was being opened. Every unit must still be decoded
 *   film     a cast: video flagged as a film, 30 pictures a second with two
 *            B-frames between reference pictures, so units arrive in decoding
 *            order (0 3 1 2 6 4 5 ...). The fake decoder behaves like a real
 *            one for such a stream: it holds pictures back and gives them out
 *            in order of time, and each picture is filled with a value that
 *            says which one it is. Every picture the title keeps must carry
 *            the time that belongs to its content
 *   video    steady audio with mirrored video interleaved: 30 units a second,
 *            a keyframe with SPS and PPS every 30, one 1.5 MB unit, a size
 *            message and a reset; the audio must be unaffected
 */
#include "player.hpp"

#include "mirror_view.hpp"
#include "pcm_link.h"
#include "player.hpp"
#include "vdec.hpp"
#include "video.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
std::atomic<unsigned long> grains{0}, loud_grains{0};
std::atomic<long> first_loud_ms{-1};
timespec test_start{};

long since_start_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - test_start.tv_sec) * 1000 + (now.tv_nsec - test_start.tv_nsec) / 1000000;
}

void sleep_ms(long ms)
{
    if (ms > 0)
        usleep(static_cast<useconds_t>(ms) * 1000);
}
} // namespace

// ---- console functions the player imports --------------------------------
extern "C"
{
    int sceAudioOutInit(void)
    {
        return 0;
    }
    int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned)
    {
        return 1;
    }
    int sceAudioOutOutput(int, const void *samples)
    {
        if (samples)
        {
            const auto *s = static_cast<const std::int16_t *>(samples);
            bool loud = false;
            for (int i = 0; i < 512; ++i)
                loud = loud || s[i] > 500 || s[i] < -500;
            grains.fetch_add(1);
            if (loud)
            {
                loud_grains.fetch_add(1);
                long expected = -1;
                first_loud_ms.compare_exchange_strong(expected, since_start_ms());
            }
        }
        // One grain at 48 kHz, on an absolute schedule: sleeping a fixed time
        // per call would run slow by the cost of the call, which the player
        // rightly reads as the output falling behind.
        static timespec next{};
        if (next.tv_sec == 0)
            clock_gettime(CLOCK_MONOTONIC, &next);
        next.tv_nsec += 5333333;
        if (next.tv_nsec >= 1000000000)
        {
            next.tv_nsec -= 1000000000;
            next.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        return 256;
    }
    int sceKernelUsleep(std::uint32_t microseconds)
    {
        usleep(microseconds);
        return 0;
    }

    // ---- the decoder's imports: a fake that accepts the documented call
    // order and checks what it is handed. Layouts are not checked here (the
    // real ones are pinned by static_asserts in vdec.cpp); offsets below are
    // those of the structures in that file.
    int sceSysmoduleLoadModule(std::uint32_t id)
    {
        return id == 207 ? 0 : -1;
    }
    std::int64_t sceKernelGetDirectMemorySize(void)
    {
        return 1LL << 34;
    }
    std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t length, std::size_t alignment,
                                               int type, std::int64_t *start)
    {
        static std::int64_t next = 0x100000;
        if (alignment != 0x4000 || type != 12 || length % 0x4000 != 0)
            return -2;
        *start = next;
        next += static_cast<std::int64_t>(length);
        return 0;
    }
    std::int32_t sceKernelMapDirectMemory(void **address, std::size_t length, int, int, std::int64_t, std::size_t)
    {
        *address = std::aligned_alloc(0x4000, length);
        return *address ? 0 : -3;
    }
    std::int32_t sceKernelMapNamedFlexibleMemory(void **address, std::size_t length, int, int, const char *)
    {
        *address = std::aligned_alloc(0x4000, length);
        return *address ? 0 : -3;
    }
    std::int32_t sceVideodec2QueryComputeMemoryInfo(std::uint64_t *memory)
    {
        if (memory[0] != 24)
            return -10;
        memory[1] = 0x12345; // deliberately not 16K aligned
        return 0;
    }
    std::int32_t sceVideodec2AllocateComputeQueue(const std::uint64_t *config, const std::uint64_t *memory, void **queue)
    {
        if (config[0] != 16 || memory[1] % 0x4000 != 0 || memory[2] == 0)
            return -11;
        *queue = reinterpret_cast<void *>(0x1111);
        return 0;
    }
    std::int32_t sceVideodec2QueryDecoderMemoryInfo(const std::uint64_t *config, std::uint64_t *memory)
    {
        if (config[0] != 72 || memory[0] != 72 || config[5] != 0x1111)
            return -12;
        memory[1] = 0x300001; // cpu
        memory[3] = 0x800001; // gpu
        memory[5] = 0x100001; // shared
        memory[7] = 0x340001; // one frame: 2048 x 1088 x 1.5 and a little
        return 0;
    }
    std::int32_t sceVideodec2CreateDecoder(const std::uint64_t *, const std::uint64_t *memory, void **decoder)
    {
        if (memory[2] == 0 || memory[4] == 0 || memory[6] == 0 || memory[3] % 0x4000 != 0 || memory[5] % 0x4000 != 0)
            return -13;
        *decoder = reinterpret_cast<void *>(0x2222);
        return 0;
    }
    // Like the console's decoder: once it has seen a parameter set it refuses
    // a different one (0x811D0300) until it is reset.
    std::atomic<std::uint32_t> fake_latched_sps{0};
    std::atomic<unsigned long> fake_fail_at{0}; // the decode call that fails once, 0 for none
    std::atomic<unsigned> fake_deletes{0};
    std::int32_t sceVideodec2Reset(void *decoder)
    {
        fake_latched_sps.store(0);
        return decoder == reinterpret_cast<void *>(0x2222) ? 0 : -14;
    }
    std::int32_t sceVideodec2DeleteDecoder(void *)
    {
        fake_deletes.fetch_add(1);
        fake_latched_sps.store(0);
        return 0;
    }
    std::atomic<unsigned long> fake_decodes{0};
    // Film mode: a decoder that reorders. It keeps the times (the pts field of
    // the input) of what it has taken, and once it holds more than two gives
    // out the earliest; the picture's luma says which time it is.
    std::atomic<bool> fake_film{false};
    static std::uint64_t fake_held_pts[8];
    static unsigned fake_held_count = 0;
    static const std::uint64_t film_base_us = 123456789ULL; // the test's timeline origin, in the decoder's microseconds
    static std::uint8_t film_luma(std::uint64_t pts_us)
    {
        const std::uint64_t index = (pts_us - film_base_us + 16666) / 33333; // which picture of the film
        return static_cast<std::uint8_t>(40 + index % 200);
    }
    // A 1920x1088 NV12 picture at a pitch of 2048 in the frame slot: mid grey
    // with a pure red left half (BT.709: Y 63, U 102, V 240), and junk in the
    // padding that must never reach the screen.
    static void fake_picture(std::uint64_t *frame, std::uint8_t *output)
    {
        auto *buffer = reinterpret_cast<std::uint8_t *>(frame[1]);
        std::memset(buffer, 0xee, 2048 * 1088 * 3 / 2);
        for (int row = 0; row < 1088; ++row)
            for (int x = 0; x < 1920; ++x)
                buffer[row * 2048 + x] = x < 960 ? 63 : 128;
        std::uint8_t *chroma = buffer + 2048 * 1088;
        for (int row = 0; row < 544; ++row)
            for (int x = 0; x < 1920; x += 2)
            {
                chroma[row * 2048 + x] = x < 960 ? 102 : 128;
                chroma[row * 2048 + x + 1] = x < 960 ? 240 : 128;
            }
        const std::uint32_t values[4] = {1920, 2048, 1088, 0}; // width, pitch, height, reserved
        std::memcpy(output + 16, values, sizeof(values));
        std::memcpy(output + 32, &frame[1], 8); // buffer
        const std::uint32_t pitch_bytes = 2048;
        std::memcpy(output + 52, &pitch_bytes, 4);
    }
    std::int32_t sceVideodec2Decode(void *decoder, std::uint64_t *input, std::uint64_t *frame, std::uint8_t *output)
    {
        const auto *unit = reinterpret_cast<const std::uint8_t *>(input[1]);
        if (decoder != reinterpret_cast<void *>(0x2222) || input[0] != 48 || frame[0] != 32 || frame[1] == 0 ||
            input[2] < 5 || unit[0] != 0 || unit[1] != 0 || unit[2] != 0 || unit[3] != 1)
            return -15;
        fake_decodes.fetch_add(1);
        if (input[2] >= 9 && unit[4] == 0x67)
        {
            std::uint32_t sps = 0;
            std::memcpy(&sps, unit + 4, 4);
            const std::uint32_t latched = fake_latched_sps.exchange(sps);
            if (latched != 0 && latched != sps)
            {
                fake_latched_sps.store(latched);
                return static_cast<std::int32_t>(0x811D0300);
            }
        }
        if (fake_fail_at.load() != 0 && fake_decodes.load() == fake_fail_at.load())
            return static_cast<std::int32_t>(0x811D0001);
        if (fake_film.load())
        {
            fake_held_pts[fake_held_count++] = input[3];
            output[9] = 0;
            if (fake_held_count <= 2)
            {
                output[8] = 0; // held back
                return 0;
            }
            unsigned earliest = 0;
            for (unsigned i = 1; i < fake_held_count; ++i)
                if (fake_held_pts[i] < fake_held_pts[earliest])
                    earliest = i;
            const std::uint64_t pts = fake_held_pts[earliest];
            fake_held_pts[earliest] = fake_held_pts[--fake_held_count];
            output[8] = 1;
            fake_picture(frame, output);
            std::memset(reinterpret_cast<void *>(frame[1]), film_luma(pts), 2048 * 1088); // the whole luma plane
            return 0;
        }
        // Every third picture is held back, as a real decoder may: Flush delivers it.
        output[8] = fake_decodes.load() % 3 == 0 ? 0 : 1; // valid
        output[9] = 0;                                     // error
        fake_picture(frame, output);
        return 0;
    }
    std::int32_t sceVideodec2Flush(void *, std::uint64_t *frame, std::uint8_t *output)
    {
        if (frame[1] == 0)
            return -16;
        output[8] = 1;
        output[9] = 0;
        fake_picture(frame, output);
        return 0;
    }
}

namespace
{
std::string pattern;
std::string hello_name; // what the title sent as the console's name
bool hello_ok = false;
std::atomic<unsigned long> loud_before_hold{0}, loud_in_hold{0}; // "hold": grains with sound
std::atomic<long> buffered_after_resume{-1}; // "resume": buffer 1.5 s after the sound came back

bool send_all(int fd, const void *data, std::size_t length)
{
    const auto *p = static_cast<const std::uint8_t *>(data);
    while (length > 0)
    {
        const ssize_t n = send(fd, p, length, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        p += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

std::uint8_t audio_flags = 0; // PCM_LINK_FLAG_LIVE for the live patterns
std::uint8_t video_flags = 0; // PCM_LINK_FLAG_ORDERED for the film pattern

bool send_message(int fd, std::uint8_t type, const void *body, std::uint32_t length)
{
    std::uint8_t header[PCM_LINK_HEADER_SIZE];
    pcm_link_header(header, type, type == PCM_LINK_AUDIO ? audio_flags : (type == PCM_LINK_VIDEO ? video_flags : 0), length);
    return send_all(fd, header, sizeof(header)) && (length == 0 || send_all(fd, body, length));
}

// 383 frames per message: one 352 sample ALAC packet resampled to 48 kHz.
constexpr int kFrames = 383;
constexpr double kMessageMs = 1000.0 * kFrames / PCM_LINK_SAMPLE_RATE;

void *fake_payload(void *)
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(PCM_LINK_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
    {
        std::perror("fake payload bind");
        std::exit(2);
    }
    const int fd = accept(listener, nullptr, nullptr);
    // The title introduces itself first: a hello carrying the console's name.
    {
        std::uint8_t hello[PCM_LINK_HEADER_SIZE + PCM_LINK_HELLO_MAX] = {};
        std::size_t got = 0;
        while (got < PCM_LINK_HEADER_SIZE)
        {
            const ssize_t n = recv(fd, hello + got, PCM_LINK_HEADER_SIZE - got, 0);
            if (n <= 0)
                break;
            got += static_cast<std::size_t>(n);
        }
        const std::size_t length = hello[4];
        while (got == PCM_LINK_HEADER_SIZE && hello_name.size() < length)
        {
            char byte = 0;
            if (recv(fd, &byte, 1, 0) <= 0)
                break;
            hello_name.push_back(byte);
        }
        hello_ok = got == PCM_LINK_HEADER_SIZE && hello[0] == 'M' && hello[1] == 'P' && hello[2] == PCM_LINK_HELLO &&
                   hello[3] == 0 && hello_name.empty();
    }
    // The payload answers with the name it is listed under.
    {
        static const char listed[] = "Mirage - Test PS5 Pro";
        send_message(fd, PCM_LINK_NAME, listed, sizeof(listed) - 1);
    }
    clock_gettime(CLOCK_MONOTONIC, &test_start);

    std::uint8_t body[PCM_LINK_AUDIO_PREFIX + kFrames * 4];
    double phase = 0.0;
    // An arbitrary origin, as on the console. "music" is what an audio-only AirPlay stream looks like
    // there: the timeline starts at exactly 0 (the payload counts RTP ticks from its first packet).
    std::int64_t timeline = pattern == "music" ? 0 : 123456789000;
    bool stalled = false, flushed = false, hold_sent = false;
    audio_flags = pattern == "resume" ? PCM_LINK_FLAG_LIVE : 0;
    video_flags = pattern == "film" ? PCM_LINK_FLAG_ORDERED : 0;
    fake_film.store(pattern == "film");
    int video_sent = 0;
    static std::uint8_t unit[PCM_LINK_VIDEO_PREFIX + 1536 * 1024];
    if (pattern == "rotate")
        fake_fail_at.store(100);
    if (pattern == "video" || pattern == "rotate" || pattern == "late" || pattern == "film")
    {
        std::uint8_t size_body[PCM_LINK_VIDEO_SIZE_BODY] = {0};
        const std::uint32_t values[4] = {1170, 2532, 886, 1920};
        for (int i = 0; i < 4; ++i)
            std::memcpy(size_body + 4 * i, &values[i], 4);
        send_message(fd, PCM_LINK_VIDEO_SIZE, size_body, sizeof(size_body));
    }
    for (int message = 0; message * kMessageMs < 6000.0; ++message)
    {
        const double due_ms = message * kMessageMs; // when a steady sender would send it
        double send_at_ms = due_ms;
        if (pattern == "burst" && due_ms < 1000.0)
            send_at_ms = 0.0;
        if (pattern == "stall" && due_ms >= 2000.0 && due_ms < 2100.0)
            send_at_ms = 2100.0;
        if (pattern == "gap" && due_ms >= 2000.0 && due_ms < 2400.0)
            send_at_ms = 2400.0;
        if (pattern == "hold" && due_ms >= 2000.0)
        {
            send_at_ms = due_ms + 1000.0;
            if (!hold_sent)
            {
                const std::uint8_t on = 1, off = 0;
                hold_sent = true;
                sleep_ms(2000 - since_start_ms());
                send_message(fd, PCM_LINK_HOLD, &on, 1);
                loud_before_hold = loud_grains.load();
                sleep_ms(2900 - since_start_ms()); // well inside the hold: what was buffered must not have played
                loud_in_hold = loud_grains.load() - loud_before_hold;
                sleep_ms(3000 - since_start_ms());
                send_message(fd, PCM_LINK_HOLD, &off, 1);
            }
        }
        if (pattern == "resume")
        {
            // Nothing is sent for the second from 2 s to 3 s, and the sender's clock runs on meanwhile.
            if (due_ms >= 2000.0)
                send_at_ms = due_ms + 1000.0;
            if (due_ms >= 2000.0 && due_ms < 2300.0)
                send_at_ms = 3300.0; // the first 300 ms after the pause: late, in one burst
            if (send_at_ms >= 4800.0 && buffered_after_resume < 0)
                buffered_after_resume = static_cast<long>(mirage::player_status().buffered_ms);
        }
        if (pattern == "flush" && !flushed && due_ms >= 2000.0)
        {
            flushed = true;
            send_message(fd, PCM_LINK_FLUSH, nullptr, 0);
            timeline += 30LL * 1000000000LL; // the sender seeks
        }
        (void)stalled;
        sleep_ms(static_cast<long>(send_at_ms) - since_start_ms());

        while ((pattern == "video" || pattern == "rotate" || pattern == "late" || pattern == "film") &&
               (video_sent * (1000.0 / 30.0) <= due_ms || (pattern == "late" && video_sent < 150)))
        {
            // Start codes and NAL headers as in an H.264 stream; the payload bytes are filler.
            std::uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x64, 0, 0x28};
            if (pattern == "rotate" && video_sent >= 60 && video_sent < 120)
                sps[7] = 0x29; // another sequence: the turned picture
            static const std::uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80};
            const bool key = pattern == "late" ? video_sent == 0 : video_sent % 30 == 0;
            std::size_t at = PCM_LINK_VIDEO_PREFIX;
            std::memset(unit, 0, PCM_LINK_VIDEO_PREFIX);
            // On the audio's timeline (same origin, same rate), as in a mirroring session.
            // A film's units come in decoding order: of every three pictures the last is sent first.
            std::int64_t shown = video_sent; // which picture, in order of showing, this unit holds
            if (pattern == "film" && video_sent > 0)
                shown = (video_sent - 1) / 3 * 3 + (((video_sent - 1) % 3 == 0) ? 3 : (video_sent - 1) % 3);
            pcm_link_put_time(unit, 123456789000LL + shown * 33333333LL);
            unit[8] = PCM_LINK_CODEC_H264;
            if (key)
            {
                std::memcpy(unit + at, sps, sizeof(sps));
                at += sizeof(sps);
                std::memcpy(unit + at, pps, sizeof(pps));
                at += sizeof(pps);
            }
            const std::uint8_t start[] = {0, 0, 0, 1, static_cast<std::uint8_t>(key ? 0x65 : 0x41)};
            std::memcpy(unit + at, start, sizeof(start));
            at += sizeof(start);
            const std::size_t filler = video_sent == 60 ? 1536 * 1024 - 64 : (key ? 60000 : 8000);
            std::memset(unit + at, 0x5a, filler);
            at += filler;
            if (video_sent == 90 && pattern == "video")
                send_message(fd, PCM_LINK_VIDEO_RESET, nullptr, 0);
            if (!send_message(fd, PCM_LINK_VIDEO, unit, static_cast<std::uint32_t>(at)))
                break;
            ++video_sent;
        }
        // A live stream is stamped with the sender's clock, which does not stop for a pause.
        if (pattern == "resume")
            timeline = 123456789000LL + static_cast<std::int64_t>((due_ms >= 2000.0 ? due_ms + 1000.0 : due_ms) * 1e6);
        pcm_link_put_time(body, pattern == "frozen" ? 0 : timeline);
        for (int i = 0; i < kFrames; ++i)
        {
            const auto sample = static_cast<std::int16_t>(std::sin(phase) * 8000.0);
            phase += 2.0 * M_PI * 440.0 / PCM_LINK_SAMPLE_RATE;
            std::memcpy(body + PCM_LINK_AUDIO_PREFIX + 4 * i, &sample, 2);
            std::memcpy(body + PCM_LINK_AUDIO_PREFIX + 4 * i + 2, &sample, 2);
        }
        timeline += static_cast<std::int64_t>(kFrames) * 1000000000LL / PCM_LINK_SAMPLE_RATE;
        if (!send_message(fd, PCM_LINK_AUDIO, body, sizeof(body)))
            break;
    }
    sleep_ms(1200); // let the buffer play out
    close(fd);
    close(listener);
    return nullptr;
}
} // namespace

// Stands in for the screen during the film pattern: 60 times a second it
// takes the picture that is due by the sound's schedule, as the title's
// drawing code does. Without it nothing uses pictures up, and the decoder
// (rightly) stops making more. It checks each picture it is given.
std::atomic<bool> screen_running{false};
std::atomic<unsigned> screen_shown{0}, screen_wrong{0}, screen_backwards{0};

void *fake_screen(void *)
{
    std::int64_t last_time = 0;
    unsigned long last_serial = 0;
    while (screen_running.load())
    {
        std::int64_t offset = 0;
        mirage::Picture picture{};
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const std::int64_t due = mirage::audio_schedule_offset(offset)
                                     ? static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec - offset
                                     : INT64_MAX;
        if (mirage::vdec_lock_picture(picture, due))
        {
            if (picture.serial != last_serial)
            {
                const std::uint64_t index =
                    static_cast<std::uint64_t>((picture.time_ns - 123456789000LL + 16666666) / 33333333);
                screen_shown.fetch_add(1);
                if (picture.luma[0] != static_cast<std::uint8_t>(40 + index % 200) ||
                    picture.luma[picture.width * picture.height - 1] != picture.luma[0])
                    screen_wrong.fetch_add(1);
                if (picture.time_ns < last_time)
                    screen_backwards.fetch_add(1);
                last_time = picture.time_ns;
                last_serial = picture.serial;
            }
            mirage::vdec_unlock_picture();
        }
        usleep(16000);
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    pattern = argc > 1 ? argv[1] : "steady";
    pthread_t payload;
    pthread_create(&payload, nullptr, &fake_payload, nullptr);
    usleep(100000);
    if (!mirage::vdec_start() || !mirage::start_player())
    {
        std::puts("FAIL: threads did not start");
        return 1;
    }
    pthread_t screen{};
    if (pattern == "film")
    {
        screen_running.store(true);
        pthread_create(&screen, nullptr, &fake_screen, nullptr);
    }
    pthread_join(payload, nullptr);
    if (pattern == "film")
    {
        screen_running.store(false);
        pthread_join(screen, nullptr);
    }

    const mirage::PlayerStatus status = mirage::player_status();
    const double loud_seconds = loud_grains.load() * 256.0 / PCM_LINK_SAMPLE_RATE;
    std::printf("%-7s loud %.2f s of 6.00 sent, first sound at %ld ms, dry %u, late %u, restart %u, bad %lu, "
                "received %lu s\n",
                pattern.c_str(), loud_seconds, first_loud_ms.load(), status.underruns, status.late_drops,
                status.restarts, status.bad_messages, status.received_seconds);

    // What each pattern must achieve. In all of them the title must have introduced itself.
    bool ok = status.bad_messages == 0 && hello_ok && std::string(status.listed_name) == "Mirage - Test PS5 Pro";
    if (std::string(status.listed_name) != "Mirage - Test PS5 Pro")
        std::printf("        listed name wrong: \"%s\"\n", status.listed_name);
    if (!hello_ok)
        std::printf("        hello wrong or missing: \"%s\"\n", hello_name.c_str());
    if (pattern == "steady" || pattern == "burst" || pattern == "music")
        ok = ok && loud_seconds > 5.7 && status.underruns <= 1 && status.late_drops == 0 && status.restarts == 0;
    else if (pattern == "stall")
        ok = ok && loud_seconds > 5.7 && status.underruns <= 1 && status.restarts == 0;
    else if (pattern == "gap") // one dry spell in the middle, one at the end, and sound again after the first
        ok = ok && loud_seconds > 5.3 && status.underruns == 2 && status.restarts == 0;
    else if (pattern == "hold")
    {
        std::printf("        grains with sound during the hold: %lu (a grain is 5.3 ms)\n", loud_in_hold.load());
        // At most the grain or two in flight when the hold arrived; and all six seconds in the end.
        ok = ok && loud_in_hold.load() <= 3 && loud_seconds > 5.9 && status.underruns <= 1 && status.late_drops == 0 &&
             status.restarts == 0;
    }
    else if (pattern == "resume")
    {
        std::printf("        buffer 1.5 s after the resume: %ld ms, reschedules %u\n", buffered_after_resume.load(),
                    status.reschedules);
        // About 150 ms again, not 450; the late 300 ms were skipped, the rest played.
        ok = ok && buffered_after_resume.load() >= 0 && buffered_after_resume.load() < 260 && loud_seconds > 5.2 &&
             status.restarts == 0;
    }
    else if (pattern == "flush")
        ok = ok && loud_seconds > 5.0 && status.restarts == 0;
    else if (pattern == "frozen")
        ok = ok && loud_seconds > 3.0; // degraded, but it must make sound
    else if (pattern == "film")
    {
        const mirage::VdecStatus vdec = mirage::vdec_status();
        // Walk the pictures the title still holds, oldest first: each one's content must be the picture
        // that belongs to its time, and the times must rise.
        unsigned checked = 0, wrong = 0;
        std::int64_t previous = 0;
        for (;;)
        {
            mirage::Picture picture{};
            if (!mirage::vdec_lock_picture(picture, previous == 0 ? INT64_MIN : previous + 1))
                break;
            const std::int64_t time = picture.time_ns;
            const std::uint64_t index = static_cast<std::uint64_t>((time - 123456789000LL + 16666666) / 33333333);
            const bool content_ok = picture.luma[0] == static_cast<std::uint8_t>(40 + index % 200) &&
                                    picture.luma[picture.width * picture.height - 1] == picture.luma[0];
            mirage::vdec_unlock_picture();
            if (time == previous)
                break; // nothing newer left
            ++checked;
            if (!content_ok || time < previous)
                ++wrong;
            previous = time;
        }
        std::printf("        decoder: pictures %lu, held %lu, reordered %lu, errors %lu, dropped %lu\n", vdec.pictures,
                    vdec.held, vdec.reordered, vdec.errors, vdec.dropped);
        std::printf("        screen: %u pictures shown, %u with the wrong content for their time, %u steps backwards; "
                    "%u left over checked, %u wrong\n",
                    screen_shown.load(), screen_wrong.load(), screen_backwards.load(), checked, wrong);
        // 180 units, two still inside the decoder at the end; nearly all of the rest reach the screen.
        ok = ok && loud_seconds > 5.5 && vdec.errors == 0 && vdec.dropped == 0 && vdec.held == 2 &&
             vdec.pictures == 178 && screen_shown.load() >= 150 && screen_wrong.load() == 0 &&
             screen_backwards.load() == 0 && wrong == 0;
    }
    else if (pattern == "late")
    {
        const mirage::VdecStatus vdec = mirage::vdec_status();
        const mirage::VideoStatus video = mirage::video_status();
        std::printf("        video units %lu; decoder: pictures %lu, errors %lu, dropped %lu, queue high %u\n", video.units,
                    vdec.pictures, vdec.errors, vdec.dropped, vdec.queue_high);
        // 150 units at once (the first five seconds), then one more every 33 ms up to the six second mark:
        // 180 in all. One keyframe, and every later picture needs the one before, so any drop loses the rest.
        ok = ok && loud_seconds > 5.5 && video.units == 180 && vdec.pictures == 180 && vdec.errors == 0 &&
             vdec.dropped == 0 && vdec.queue_high > 16;
    }
    else if (pattern == "rotate")
    {
        const mirage::VdecStatus vdec = mirage::vdec_status();
        std::printf("        decoder: pictures %lu, errors %lu (last %08x), dropped %lu, sequence changes %u, re-created %u, "
                    "reset result %d\n",
                    vdec.pictures, vdec.errors, static_cast<unsigned>(vdec.last_error), vdec.dropped,
                    vdec.sequence_changes, vdec.recreations, vdec.reset_result);
        // Two changes of sequence, each met with a reset and so no refusal; the one forced failure costs at
        // most the rest of its second (29 units) and no new decoder.
        ok = ok && loud_seconds > 5.5 && vdec.sequence_changes == 2 && vdec.errors == 1 &&
             static_cast<unsigned>(vdec.last_error) == 0x811D0001u && vdec.recreations == 0 && vdec.reset_result == 0 &&
             vdec.pictures >= 150 && vdec.pictures + vdec.dropped + vdec.errors == 180;
    }
    else if (pattern == "video")
    {
        const mirage::VideoStatus video = mirage::video_status();
        std::printf("        video units %lu, key %lu, sps %lu, largest %u bytes, resets %u, %ux%u, span %lld ms\n",
                    video.units, video.keyframes, video.parameter_sets, video.largest_unit, video.resets,
                    video.width, video.height, static_cast<long long>(video.time_span_ms));
        const mirage::VdecStatus vdec = mirage::vdec_status();
        std::printf("        decoder %s%s, pictures %lu, none %lu, errors %lu, dropped %lu, %ux%u pitch %u, queue high %u\n",
                    mirage::vdec_stage_name(vdec.stage), vdec.failed ? " FAILED" : "", vdec.pictures, vdec.no_picture,
                    vdec.errors, vdec.dropped, vdec.width, vdec.height, vdec.pitch, vdec.queue_high);
        // Every unit is decodable here: the stream starts on a keyframe and the reset is followed by one.
        // Which picture is due: the last twelve are kept (numbers 169 to 180, times of index 168 to 179).
        mirage::Picture due_picture{};
        const std::int64_t video_base = 123456789000LL;
        bool due_ok = mirage::vdec_lock_picture(due_picture, video_base + 175 * 33333333LL + 1);
        const unsigned long due_first = due_ok ? due_picture.serial : 0;
        if (due_ok)
            mirage::vdec_unlock_picture();
        due_ok = due_ok && mirage::vdec_lock_picture(due_picture, video_base + 170 * 33333333LL);
        const unsigned long due_earlier = due_ok ? due_picture.serial : 0; // nothing newer is due: stays put
        if (due_ok)
            mirage::vdec_unlock_picture();
        std::printf("        due: picture %lu for the time of the 176th, then %lu for an earlier time\n", due_first,
                    due_earlier);
        ok = ok && due_ok && due_first == 176 && due_earlier == 176;
        // Draw the last picture twice: as mirrored (visible part 1920x1080 on a 1920x1080 frame) and as a
        // portrait crop (the left 608 columns), which must be centred with black on both sides.
        static std::uint32_t canvas[1920 * 1080];
        const auto to_canvas = [](void *context, unsigned y, const std::uint32_t *row) {
            std::memcpy(static_cast<std::uint32_t *>(context) + static_cast<std::size_t>(y) * 1920, row, 1920 * 4);
        };
        std::memset(canvas, 0x11, sizeof(canvas)); // every pixel must be written
        const bool drew = mirage::draw_mirror(to_canvas, canvas, 1920, 1080, 1920, 1080);
        const std::uint32_t red = canvas[540 * 1920 + 100], grey = canvas[540 * 1920 + 1800];
        const std::uint32_t last = canvas[1079 * 1920 + 1919];
        const bool drew_portrait = mirage::draw_mirror(to_canvas, canvas, 1920, 1080, 608, 1080);
        bool all_written = true;
        for (std::size_t i = 0; i < 1920 * 1080; ++i)
            all_written = all_written && canvas[i] != 0x11111111u;
        const std::uint32_t bar = canvas[540 * 1920 + 10], middle = canvas[540 * 1920 + 960];
        std::printf("        drawn %d/%d: red %08x, grey %08x, last %08x; portrait bar %08x, middle %08x\n", drew,
                    drew_portrait, red, grey, last, bar, middle);
        // 0xAABBGGRR: red is ff0000ff give or take rounding, grey 808080-ish, and no 0xee padding anywhere.
        ok = ok && all_written && drew && drew_portrait && (red & 0xff) > 0xf0 && (red >> 8 & 0xffff) < 0x1010 &&
             (grey & 0xff) > 0x78 && (grey & 0xff) < 0x88 && last == grey && bar == 0xff000000u && middle == red;
        ok = ok && vdec.stage == mirage::VdecStage::ready && !vdec.failed && vdec.errors == 0 && vdec.no_picture == 0 &&
             vdec.pictures + vdec.dropped == video.units && vdec.pictures >= 170 && vdec.width == 1920 &&
             vdec.height == 1088 && vdec.pitch == 2048;
        // The audio bound is looser here: with megabyte units in the stream the test's own end cuts it close.
        ok = ok && loud_seconds > 5.5 && status.late_drops == 0 && status.restarts == 0 && video.units >= 178 &&
             video.units <= 182 && video.keyframes == 6 && video.parameter_sets == 6 &&
             video.largest_unit > 1500000 && video.resets == 1 && video.width == 886 && video.height == 1920 &&
             !video.h265;
    }
    std::puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
