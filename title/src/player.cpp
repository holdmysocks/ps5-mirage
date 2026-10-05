/*
 * Mirage - title: starts the payload and plays the PCM it sends over loopback.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two threads.
 *
 * The link thread connects to the payload on 127.0.0.1 (a title may connect
 * out but not listen). If nothing answers it starts the payload, by sending
 * /app0/mirage.elf to the ELF loader on 127.0.0.1:9021, and keeps trying. It
 * then reads messages into a ring buffer.
 *
 * The audio thread owns AudioOut. sceAudioOutOutput blocks for one grain, so
 * it paces the loop, but what to play is decided by time: every audio message
 * carries the time it is due on the sender's timeline (pcm_link.h). When a
 * stream starts, the first frame is scheduled kDelayMs after it arrived; from
 * then on each frame plays at its own time plus that same offset. Audio that
 * arrives in bursts or stalls briefly therefore does not change the delay,
 * and the buffer absorbs it. If the buffer still runs dry, the next audio
 * starts a new schedule.
 */
#include "player.hpp"

#include "pcm_link.h" // shared with the payload: src\pcm_link.h, copied in by the build
#include "video.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceKernelUsleep(std::uint32_t microseconds);
}

namespace mirage
{
void set_hold(bool hold) noexcept; // below, with the schedule it changes

namespace
{
constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);
constexpr int kSystemUser = 0xff;
constexpr int kPortMain = 0;
constexpr unsigned kFormatS16Stereo = 1;
constexpr unsigned kGrainFrames = 256;
constexpr unsigned kRate = PCM_LINK_SAMPLE_RATE;
constexpr unsigned kRingFrames = kRate * 3; // three seconds
constexpr std::int64_t kNsPerSecond = 1000000000;
// The first frame plays this long after it arrives. Measured on the console
// (2026-10-05): packets arrive within about 15 ms of their time, the odd one
// 50 ms late, so 150 ms is ample; mirrored pictures wait the same time.
constexpr std::int64_t kDelayNs = 150 * 1000000LL;
constexpr std::int64_t kToleranceNs = 40 * 1000000LL;  // schedule error left alone
constexpr std::int64_t kJoinNs = 60 * 1000000LL;       // a larger step in message times starts a new stream
constexpr unsigned kDistrustRestarts = 3;
constexpr std::int64_t kSameClockNs = 1000000000LL;  // a baseline further off than this is another clock
constexpr std::int64_t kCreepNs = 200000;            // the baseline may rise this much a second (clock drift)
constexpr std::int64_t kRescheduleNs = 80 * 1000000LL; // a live schedule this far behind its baseline is redone

constexpr std::uint16_t kLoaderPort = 9021;
constexpr const char *kPayloadPath = "/app0/mirage.elf";
constexpr unsigned kMaxLaunches = 3;      // per run of the title
constexpr unsigned kRelaunchSeconds = 20; // wait this long for a started payload to answer

constexpr std::int64_t frames_to_ns(std::int64_t frames) noexcept
{
    return frames * kNsPerSecond / kRate;
}

constexpr std::int64_t ns_to_frames(std::int64_t ns) noexcept
{
    return ns * kRate / kNsPerSecond;
}

// The player's clock. Monotonic on purpose: schedules are built on it, and a
// wall clock that the system corrects by a third of a second would make the
// player wait (or skip) by as much. Only differences are ever used.
std::int64_t now_ns() noexcept
{
    timespec ts{};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * kNsPerSecond + ts.tv_nsec;
}

// Everything below is guarded by ring_lock.
pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
std::int16_t ring[kRingFrames * 2];
unsigned ring_read = 0;         // frame index of the oldest frame
unsigned ring_count = 0;        // frames buffered
std::int64_t head_time = 0;     // sender time of the oldest frame
bool scheduled = false;         // whether play_offset is set for this stream
std::int64_t play_offset = 0;   // local play time = sender time + play_offset
bool was_playing = false;
// A hold (PCM_LINK_HOLD, a cast's pause): silence, nothing taken from the
// buffer, and time stands still for the schedule.
bool held = false;
std::int64_t held_since = 0;
// Guard against a sender whose times do not advance with its audio: after
// kDistrustRestarts restarts within a second the times are ignored for the
// rest of the connection and audio is simply played in the order it arrives.
bool trust_times = true;
// For a live stream (PCM_LINK_FLAG_LIVE) the times are the sender's wall
// clock, so "arrival time minus message time" is the clock difference plus
// the network delay: nearly constant, and the same after a pause. Its
// smallest recent value is the baseline every schedule of that stream is
// built on. Without it, a schedule started from one message that happened to
// arrive late kept that lateness for good (seen on the console: after some
// pauses the sound came back behind the picture and stayed there).
bool live_stream = false;
bool have_baseline = false;
std::int64_t baseline = 0;      // smallest arrival - time lately
std::int64_t baseline_crept = 0; // local time the baseline was last let rise
std::int64_t next_time = 0;          // where the next message continues, if times are ignored
std::int64_t restart_window = 0;     // local time the current count of restarts began
unsigned restarts_in_window = 0;

std::atomic<bool> threads_started{false};
std::atomic<bool> link_connected{false};
std::atomic<int> audio_error{0};
std::atomic<int> connect_errno{0};
std::atomic<unsigned> buffered_frames{0};
std::atomic<unsigned> underruns{0};
std::atomic<unsigned> late_drops{0};
std::atomic<unsigned> restarts{0};
std::atomic<bool> times_ignored{false};
std::atomic<unsigned> reschedules{0};
std::atomic<unsigned long> received_frames{0};
std::atomic<unsigned long> connects{0};
std::atomic<unsigned long> bad_messages{0};
std::atomic<unsigned> launches{0};
std::atomic<int> launch_error{0};

// The name the payload advertises under (PCM_LINK_NAME), for the screen.
pthread_mutex_t name_lock = PTHREAD_MUTEX_INITIALIZER;
char listed_name[PCM_LINK_NAME_MAX + 1] = ""; // guarded by name_lock
char shown_pin[PCM_LINK_PIN_MAX + 1] = "";     // the pairing code to show; guarded by name_lock

bool write_all(int fd, const void *from, std::size_t length) noexcept; // further down

// Introduces the title to the payload. The hello can carry the console's
// name, but a title has no working way to learn it on this firmware
// (sceSystemServiceParamGetString gave nothing), so it is sent empty; the
// payload finds the name itself and tells the title (PCM_LINK_NAME).
bool send_hello(int fd) noexcept
{
    std::uint8_t message[PCM_LINK_HEADER_SIZE];
    pcm_link_header(message, PCM_LINK_HELLO, 0, 0);
    return write_all(fd, message, sizeof(message));
}

void ring_clear_locked() noexcept
{
    ring_read = 0;
    ring_count = 0;
    scheduled = false;
    was_playing = false;
    held = false; // whatever was held is gone
    buffered_frames.store(0);
}

void ring_clear() noexcept
{
    pthread_mutex_lock(&ring_lock);
    ring_clear_locked();
    pthread_mutex_unlock(&ring_lock);
}

void ring_drop_locked(unsigned frames) noexcept
{
    ring_read = (ring_read + frames) % kRingFrames;
    ring_count -= frames;
    head_time += frames_to_ns(frames);
}

void ring_push(std::int64_t time, bool live, const std::int16_t *samples, unsigned frames) noexcept
{
    const std::int64_t arrived = now_ns();
    pthread_mutex_lock(&ring_lock);
    if (!trust_times)
        time = next_time;
    live_stream = live && trust_times;
    if (live_stream)
    {
        const std::int64_t lateness = arrived - time;
        if (!have_baseline || lateness - baseline > kSameClockNs || baseline - lateness > kSameClockNs)
        {
            baseline = lateness;
            baseline_crept = arrived;
            have_baseline = true;
        }
        else
        {
            if (arrived - baseline_crept > kNsPerSecond)
            {
                baseline += kCreepNs;
                baseline_crept = arrived;
            }
            if (lateness < baseline)
                baseline = lateness;
        }
    }
    else
    {
        have_baseline = false;
    }
    if (ring_count > 0)
    {
        const std::int64_t expected = head_time + frames_to_ns(ring_count);
        const std::int64_t step = time - expected;
        if (step > kJoinNs || step < -kJoinNs)
        { // not a continuation of what is buffered: a new stream
            const std::int64_t now = now_ns();
            ring_clear_locked();
            restarts.fetch_add(1);
            if (now - restart_window > kNsPerSecond)
            {
                restart_window = now;
                restarts_in_window = 0;
            }
            if (++restarts_in_window >= kDistrustRestarts)
            {
                trust_times = false;
                times_ignored.store(true);
            }
        }
    }
    if (ring_count == 0)
        head_time = time;
    next_time = time + frames_to_ns(frames);
    for (unsigned i = 0; i < frames; ++i)
    {
        if (ring_count == kRingFrames)
            ring_drop_locked(1); // full: the oldest frame goes
        const unsigned at = (ring_read + ring_count) % kRingFrames;
        ring[2 * at] = samples[2 * i];
        ring[2 * at + 1] = samples[2 * i + 1];
        ++ring_count;
    }
    buffered_frames.store(ring_count);
    pthread_mutex_unlock(&ring_lock);
}

// Fills one grain with what is due now; silence when nothing is.
void ring_pull(std::int16_t *grain) noexcept
{
    bool play = false;
    const std::int64_t now = now_ns();

    pthread_mutex_lock(&ring_lock);
    if (held)
    {
        std::memset(grain, 0, kGrainFrames * 2 * sizeof(std::int16_t));
        pthread_mutex_unlock(&ring_lock);
        return;
    }
    if (ring_count == 0)
    {
        if (was_playing)
        { // ran dry mid-stream: whatever comes next gets a fresh schedule
            underruns.fetch_add(1);
            was_playing = false;
            scheduled = false;
        }
    }
    else
    {
        if (!scheduled)
        {
            // Music: from the moment its first frame arrived. Live: from the
            // stream's baseline, however late this particular frame is.
            play_offset = (live_stream && have_baseline ? baseline : now - head_time) + kDelayNs;
            scheduled = true;
        }
        else if (live_stream && have_baseline && play_offset - (baseline + kDelayNs) > kRescheduleNs)
        { // the schedule was started from late arrivals: catch up (the overdue sound is skipped below)
            play_offset = baseline + kDelayNs;
            reschedules.fetch_add(1);
        }
        // Positive: the oldest frame should already have been played.
        std::int64_t late = now - (head_time + play_offset);
        if (late > kToleranceNs)
        {
            std::int64_t drop = ns_to_frames(late);
            if (drop > ring_count)
                drop = ring_count;
            ring_drop_locked(static_cast<unsigned>(drop));
            late_drops.fetch_add(1);
            late = 0;
        }
        // A stream starts when its first frame is due. Once it plays it keeps
        // playing grain after grain: the time the output call returns jitters
        // by several milliseconds, and correcting for that would put gaps in
        // the sound. Only an error beyond the tolerance is corrected.
        play = ring_count > 0 && late >= (was_playing ? -kToleranceNs : 0);
    }

    if (play)
    {
        const unsigned frames = ring_count < kGrainFrames ? ring_count : kGrainFrames;
        for (unsigned i = 0; i < frames; ++i)
        {
            const unsigned at = (ring_read + i) % kRingFrames;
            grain[2 * i] = ring[2 * at];
            grain[2 * i + 1] = ring[2 * at + 1];
        }
        if (frames < kGrainFrames)
            std::memset(grain + 2 * frames, 0, (kGrainFrames - frames) * 2 * sizeof(std::int16_t));
        ring_drop_locked(frames);
        was_playing = true;
    }
    else
    {
        std::memset(grain, 0, kGrainFrames * 2 * sizeof(std::int16_t));
    }
    buffered_frames.store(ring_count);
    pthread_mutex_unlock(&ring_lock);
}

void *audio_main(void *) noexcept
{
    alignas(64) static std::int16_t grain[kGrainFrames * 2];

    const int init = sceAudioOutInit();
    if (init != 0 && init != kAlreadyInitialized)
    {
        audio_error.store(init);
        return nullptr;
    }
    const int handle =
        sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrainFrames, kRate, kFormatS16Stereo);
    if (handle < 0)
    {
        audio_error.store(handle);
        return nullptr;
    }
    for (;;)
    {
        ring_pull(grain);
        if (sceAudioOutOutput(handle, grain) < 0)
            (void)sceKernelUsleep(5000); // never spin on a failing call
    }
}

bool read_all(int fd, void *into, std::size_t length) noexcept
{
    auto *p = static_cast<std::uint8_t *>(into);
    while (length > 0)
    {
        const ssize_t n = recv(fd, p, length, 0);
        if (n <= 0)
            return false;
        p += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

bool write_all(int fd, const void *from, std::size_t length) noexcept
{
    const auto *p = static_cast<const std::uint8_t *>(from);
    while (length > 0)
    {
        const ssize_t n = send(fd, p, length, 0);
        if (n <= 0)
            return false;
        p += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

int connect_loopback(std::uint16_t port) noexcept
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in address{};
#ifndef __linux__ // the host test (tests\host_player_test.cpp) builds this file on Linux
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
    {
        const int error = errno;
        (void)close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

void serve(int fd) noexcept
{
    static std::uint8_t body[PCM_LINK_MAX_BODY];
    // The body is not aligned for int16 access; samples are copied out. An
    // audio message is a fraction of a second; anything longer is not audio.
    constexpr unsigned kMaxAudioFrames = PCM_LINK_SAMPLE_RATE;
    static std::int16_t samples[kMaxAudioFrames * PCM_LINK_CHANNELS];
    // A code from an earlier connection is stale; the payload repeats one that still stands.
    pthread_mutex_lock(&name_lock);
    shown_pin[0] = 0;
    pthread_mutex_unlock(&name_lock);
    for (;;)
    {
        std::uint8_t header[PCM_LINK_HEADER_SIZE];
        if (!read_all(fd, header, sizeof(header)))
            return;
        const std::uint32_t length = static_cast<std::uint32_t>(header[4]) |
                                     static_cast<std::uint32_t>(header[5]) << 8 |
                                     static_cast<std::uint32_t>(header[6]) << 16 |
                                     static_cast<std::uint32_t>(header[7]) << 24;
        if (header[0] != 'M' || header[1] != 'P' || length > PCM_LINK_MAX_BODY)
        {
            bad_messages.fetch_add(1);
            return; // out of step with the stream: reconnect
        }
        if (length > 0 && !read_all(fd, body, length))
            return;
        switch (header[2])
        {
        case PCM_LINK_AUDIO:
        {
            if (length < PCM_LINK_AUDIO_PREFIX)
            {
                bad_messages.fetch_add(1);
                break;
            }
            const unsigned frames =
                (length - PCM_LINK_AUDIO_PREFIX) / (PCM_LINK_CHANNELS * sizeof(std::int16_t));
            if (frames > kMaxAudioFrames)
            {
                bad_messages.fetch_add(1);
                break;
            }
            std::memcpy(samples, body + PCM_LINK_AUDIO_PREFIX,
                        frames * PCM_LINK_CHANNELS * sizeof(std::int16_t));
            ring_push(pcm_link_get_time(body), (header[3] & PCM_LINK_FLAG_LIVE) != 0, samples, frames);
            received_frames.fetch_add(frames);
            break;
        }
        case PCM_LINK_FLUSH:
            ring_clear();
            break;
        case PCM_LINK_VIDEO:
            if (length < PCM_LINK_VIDEO_PREFIX)
            {
                bad_messages.fetch_add(1);
                break;
            }
            video_unit(pcm_link_get_time(body), body[8] == PCM_LINK_CODEC_H265, (header[3] & PCM_LINK_FLAG_ORDERED) != 0,
                       body + PCM_LINK_VIDEO_PREFIX, length - PCM_LINK_VIDEO_PREFIX);
            break;
        case PCM_LINK_VIDEO_SIZE:
            if (length == PCM_LINK_VIDEO_SIZE_BODY)
            {
                unsigned values[4];
                for (int i = 0; i < 4; ++i)
                    values[i] = static_cast<unsigned>(body[4 * i]) | static_cast<unsigned>(body[4 * i + 1]) << 8 |
                                static_cast<unsigned>(body[4 * i + 2]) << 16 |
                                static_cast<unsigned>(body[4 * i + 3]) << 24;
                video_size(values[0], values[1], values[2], values[3]);
            }
            else
            {
                bad_messages.fetch_add(1);
            }
            break;
        case PCM_LINK_VIDEO_RESET:
            video_reset();
            break;
        case PCM_LINK_HOLD:
            if (length == 1)
                set_hold(body[0] != 0);
            else
                bad_messages.fetch_add(1);
            break;
        case PCM_LINK_NAME:
            if (length <= PCM_LINK_NAME_MAX)
            {
                pthread_mutex_lock(&name_lock);
                std::memcpy(listed_name, body, length);
                listed_name[length] = 0;
                pthread_mutex_unlock(&name_lock);
            }
            else
            {
                bad_messages.fetch_add(1);
            }
            break;
        case PCM_LINK_PIN:
            if (length <= PCM_LINK_PIN_MAX)
            {
                pthread_mutex_lock(&name_lock);
                std::memcpy(shown_pin, body, length);
                shown_pin[length] = 0;
                pthread_mutex_unlock(&name_lock);
            }
            else
            {
                bad_messages.fetch_add(1);
            }
            break;
        case PCM_LINK_STOP:
        default:
            break; // what is buffered plays out
        }
    }
}

// Sends the packaged payload to the ELF loader. The loader socket is closed
// afterwards: the payload's output is not read here, and it ignores SIGPIPE.
void launch_payload() noexcept
{
    static std::uint8_t chunk[65536];
    launches.fetch_add(1);
    const int file = open(kPayloadPath, O_RDONLY);
    if (file < 0)
    {
        launch_error.store(errno ? errno : -1);
        return;
    }
    const int loader = connect_loopback(kLoaderPort);
    if (loader < 0)
    {
        launch_error.store(errno ? errno : -1);
        (void)close(file);
        return;
    }
    int error = 0;
    for (;;)
    {
        const ssize_t n = read(file, chunk, sizeof(chunk));
        if (n == 0)
            break;
        if (n < 0 || !write_all(loader, chunk, static_cast<std::size_t>(n)))
        {
            error = errno ? errno : -1;
            break;
        }
    }
    launch_error.store(error);
    (void)close(file);
    (void)close(loader);
}

void *link_main(void *) noexcept
{
    unsigned seconds_since_launch = kRelaunchSeconds; // so the first failure starts it
    for (;;)
    {
        const int fd = connect_loopback(PCM_LINK_PORT);
        if (fd >= 0)
        {
            connect_errno.store(0);
            connects.fetch_add(1);
            link_connected.store(true);
            if (send_hello(fd))
                serve(fd);
            link_connected.store(false);
            (void)close(fd);
            ring_clear();
            pthread_mutex_lock(&ring_lock);
            trust_times = true; // a new connection gets the benefit of the doubt
            have_baseline = false;
            restarts_in_window = 0;
            pthread_mutex_unlock(&ring_lock);
            times_ignored.store(false);
            seconds_since_launch = kRelaunchSeconds / 2; // give a stopping payload time to go
        }
        else
        {
            connect_errno.store(errno);
            if (seconds_since_launch >= kRelaunchSeconds && launches.load() < kMaxLaunches)
            {
                launch_payload();
                seconds_since_launch = 0;
            }
        }
        (void)sceKernelUsleep(1000000);
        ++seconds_since_launch;
    }
}
} // namespace

bool start_player() noexcept
{
    // Threads are not named: pthread_setname_np hangs on this platform.
    pthread_t audio, link;
    const bool ok = pthread_create(&audio, nullptr, &audio_main, nullptr) == 0 &&
                    pthread_create(&link, nullptr, &link_main, nullptr) == 0;
    threads_started.store(ok);
    return ok;
}

void set_hold(bool hold) noexcept
{
    const std::int64_t now = now_ns();
    pthread_mutex_lock(&ring_lock);
    if (hold && !held)
    {
        held = true;
        held_since = now;
    }
    else if (!hold && held)
    {
        held = false;
        if (scheduled)
            play_offset += now - held_since; // everything is due that much later
    }
    pthread_mutex_unlock(&ring_lock);
}

bool audio_schedule_offset(std::int64_t &offset_ns) noexcept
{
    const std::int64_t now = now_ns();
    pthread_mutex_lock(&ring_lock);
    const bool usable = scheduled && trust_times;
    // While held, the moment "now" on the sender's timeline does not move:
    // the picture on screen stays the one that belongs to the last sound.
    offset_ns = held ? play_offset + (now - held_since) : play_offset;
    pthread_mutex_unlock(&ring_lock);
    return usable;
}

PlayerStatus player_status() noexcept
{
    PlayerStatus status{};
    status.threads_started = threads_started.load();
    status.link_connected = link_connected.load();
    status.audio_error = audio_error.load();
    status.connect_errno = connect_errno.load();
    status.buffered_ms = buffered_frames.load() * 1000 / kRate;
    status.underruns = underruns.load();
    status.late_drops = late_drops.load();
    status.restarts = restarts.load();
    status.times_ignored = times_ignored.load();
    status.reschedules = reschedules.load();
    status.received_seconds = received_frames.load() / kRate;
    status.connects = connects.load();
    status.bad_messages = bad_messages.load();
    status.launches = launches.load();
    status.launch_error = launch_error.load();
    pthread_mutex_lock(&name_lock);
    std::memcpy(status.listed_name, listed_name, sizeof(listed_name));
    std::memcpy(status.pin, shown_pin, sizeof(shown_pin));
    pthread_mutex_unlock(&name_lock);
    return status;
}
} // namespace mirage
