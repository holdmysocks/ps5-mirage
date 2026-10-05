/*
 * Mirage - title: mirrored video arriving over the link.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "video.hpp"

#include "vdec.hpp"

#include <ctime>
#include <pthread.h>

namespace mirage
{
namespace
{
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
VideoStatus status{}; // guarded by lock
std::int64_t first_time = 0;
bool have_first_time = false;
std::int64_t second_start = 0; // local monotonic second the running counts belong to
unsigned units_this_second = 0;
unsigned long bytes_this_second = 0;

std::int64_t monotonic_seconds() noexcept
{
    timespec ts{};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

// Walks the Annex-B start codes and reports which kinds of NAL unit are there.
void classify(bool h265, const std::uint8_t *data, std::size_t length, bool &keyframe,
              bool &parameter_set) noexcept
{
    keyframe = false;
    parameter_set = false;
    for (std::size_t i = 0; i + 3 < length; ++i)
    {
        if (data[i] != 0 || data[i + 1] != 0)
            continue;
        std::size_t header;
        if (data[i + 2] == 1)
            header = i + 3;
        else if (data[i + 2] == 0 && i + 4 < length && data[i + 3] == 1)
            header = i + 4;
        else
            continue;
        if (header >= length)
            break;
        if (h265)
        {
            const unsigned type = (data[header] >> 1) & 0x3f;
            keyframe = keyframe || (type >= 16 && type <= 21); // IRAP pictures
            parameter_set = parameter_set || type == 33;       // SPS
        }
        else
        {
            const unsigned type = data[header] & 0x1f;
            keyframe = keyframe || type == 5;           // IDR slice
            parameter_set = parameter_set || type == 7; // SPS
        }
        i = header;
    }
}
} // namespace

void video_unit(std::int64_t time_ns, bool h265, bool ordered, const std::uint8_t *annexb, std::size_t length) noexcept
{
    bool keyframe = false, parameter_set = false;
    classify(h265, annexb, length, keyframe, parameter_set);
    const std::int64_t second = monotonic_seconds();
    vdec_submit(time_ns, h265, keyframe, ordered, annexb, length);

    pthread_mutex_lock(&lock);
    if (second != second_start)
    {
        // Only a second that directly follows the counted one is a full second.
        status.units_per_second = second == second_start + 1 ? units_this_second : 0;
        status.kbit_per_second =
            second == second_start + 1 ? static_cast<unsigned>(bytes_this_second * 8 / 1000) : 0;
        second_start = second;
        units_this_second = 0;
        bytes_this_second = 0;
    }
    ++units_this_second;
    bytes_this_second += length;

    ++status.units;
    status.bytes += length;
    status.h265 = h265;
    if (keyframe)
        ++status.keyframes;
    if (parameter_set)
        ++status.parameter_sets;
    if (length > status.largest_unit)
        status.largest_unit = static_cast<unsigned>(length);
    if (!have_first_time)
    {
        have_first_time = true;
        first_time = time_ns;
    }
    status.time_span_ms = (time_ns - first_time) / 1000000;
    pthread_mutex_unlock(&lock);
}

void video_size(unsigned source_width, unsigned source_height, unsigned width, unsigned height) noexcept
{
    pthread_mutex_lock(&lock);
    status.source_width = source_width;
    status.source_height = source_height;
    status.width = width;
    status.height = height;
    pthread_mutex_unlock(&lock);
}

void video_reset() noexcept
{
    vdec_reset_stream();
    pthread_mutex_lock(&lock);
    ++status.resets;
    have_first_time = false;
    status.time_span_ms = 0;
    pthread_mutex_unlock(&lock);
}

VideoStatus video_status() noexcept
{
    pthread_mutex_lock(&lock);
    VideoStatus copy = status;
    // A stream that stopped must not keep showing its last rate.
    if (monotonic_seconds() > second_start + 1)
    {
        copy.units_per_second = 0;
        copy.kbit_per_second = 0;
    }
    pthread_mutex_unlock(&lock);
    return copy;
}
} // namespace mirage
