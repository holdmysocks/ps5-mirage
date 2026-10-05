/*
 * Mirage - title: draws the latest decoded picture into the frame.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>

namespace mirage
{
struct MirrorViewStatus
{
    unsigned long drawn;      // frames drawn with a picture
    unsigned long serial;     // serial of the picture last drawn
    unsigned last_draw_ms, worst_draw_ms;
    unsigned shown_width, shown_height; // size on screen
    bool synced;              // the picture was chosen by the sound's schedule
};

// Receives one finished row of the frame: canvas_width pixels of 0xAABBGGRR.
// The frame's own memory layout is the receiver's business.
using RowWriter = void (*)(void *context, unsigned y, const std::uint32_t *row);

// Produces the whole frame, row by row from the top: the latest picture
// scaled to fit and centred, black around it. The picture is cropped to
// visible_width x visible_height first when those are not 0 (the decoder
// pads to multiples of 16). Returns false, writing nothing, when there is no
// picture.
bool draw_mirror(RowWriter write, void *context, unsigned canvas_width, unsigned canvas_height,
                 unsigned visible_width, unsigned visible_height) noexcept;
MirrorViewStatus mirror_view_status() noexcept;
} // namespace mirage
