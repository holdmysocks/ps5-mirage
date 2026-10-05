/*
 * Mirage - title: mirrored video arriving over the link.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The access units are inspected and counted, so the screen can show what the
 * sender delivers, and handed to the decoder (vdec.hpp).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mirage
{
struct VideoStatus
{
    unsigned long units;      // access units received
    unsigned long keyframes;  // units containing an IDR (H.264) or IRAP (H.265) picture
    unsigned long parameter_sets; // units containing an SPS
    unsigned long bytes;
    unsigned largest_unit;    // bytes
    unsigned resets;
    bool h265;
    unsigned source_width, source_height; // as the sender reports them
    unsigned width, height;
    unsigned units_per_second; // over the last full second
    unsigned kbit_per_second;  // over the last full second
    std::int64_t time_span_ms; // last unit's time minus the first's, since the last reset
};

// Called from the link thread.
void video_unit(std::int64_t time_ns, bool h265, bool ordered, const std::uint8_t *annexb, std::size_t length) noexcept;
void video_size(unsigned source_width, unsigned source_height, unsigned width, unsigned height) noexcept;
void video_reset() noexcept;

VideoStatus video_status() noexcept;
} // namespace mirage
