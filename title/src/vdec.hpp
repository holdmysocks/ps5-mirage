/*
 * Mirage - title: hardware video decode through libSceVideodec2.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mirage
{
// How far the decoder bring-up got; the screen shows the name and the result
// code of the call that ended it.
enum class VdecStage : int
{
    not_started,
    load_module,
    query_compute,
    allocate_compute,
    compute_queue,
    query_decoder,
    map_cpu,
    allocate_gpu,
    allocate_shared,
    create,
    allocate_pools,
    reset,
    ready,
};

struct VdecStatus
{
    VdecStage stage;       // the last stage attempted
    bool failed;           // that stage failed; result is its return value
    int result;
    unsigned long submitted;   // units handed to the decoder
    unsigned long pictures;    // units that produced a picture
    unsigned long no_picture;  // accepted, nothing came out even after a flush
    unsigned long held;        // film units the decoder took and kept for later (not a fault)
    unsigned long reordered;   // pictures that came out of the decoder before an earlier one
    unsigned long errors;      // the call failed or reported an error
    unsigned long dropped;     // not decoded: queue full, or waiting for a keyframe
    int last_error;
    unsigned width, height, pitch, pitch_bytes, format; // of the last picture
    unsigned queue_high;       // most units ever waiting
    unsigned worst_decode_ms;
    unsigned worst_copy_ms;    // copying a picture out of the decoder's memory
    unsigned sequence_changes; // the stream's SPS changed (rotation): decoder reset first
    unsigned recreations;      // decoder deleted and created again after repeated errors
    int reset_result;          // result of the last reset or re-creation, 0 if fine
};

const char *vdec_stage_name(VdecStage stage) noexcept;

// Starts the decoder thread, which brings the decoder up and then decodes
// what vdec_submit queues. With MIRAGE_VDEC_BRINGUP_ONLY the thread stops
// after the bring-up and every submitted unit is counted as dropped.
bool vdec_start() noexcept;
// One Annex-B access unit. Copied. Called from the link thread; returns at
// once unless the queue is full, when it waits a while for room. ordered: the
// unit belongs to a film (PCM_LINK_FLAG_ORDERED), not a mirrored screen.
void vdec_submit(std::int64_t time_ns, bool h265, bool keyframe, bool ordered, const std::uint8_t *annexb,
                 std::size_t length) noexcept;
// The stream ended or restarts: forget queued units, wait for a keyframe.
void vdec_reset_stream() noexcept;
VdecStatus vdec_status() noexcept;

// A decoded picture, copied out of the decoder's memory: NV12, luma then
// interleaved chroma at half height, both width bytes per row.
struct Picture
{
    const std::uint8_t *luma;
    const std::uint8_t *chroma;
    unsigned width, height;
    std::int64_t time_ns; // the sender's time of the access unit it came from
    unsigned long serial; // counts pictures from 1
};
// The picture to show now: the newest one whose time is not after due_ns
// (pass INT64_MAX for simply the newest), or, if none is due yet, the one
// shown last. Older pictures are forgotten. Holds the picture still until
// vdec_unlock_picture. False if there is none (nothing to unlock then).
bool vdec_lock_picture(Picture &picture, std::int64_t due_ns) noexcept;
void vdec_unlock_picture() noexcept;
} // namespace mirage
