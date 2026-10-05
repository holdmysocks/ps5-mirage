/*
 * Mirage - title: draws the latest decoded picture into the frame.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A plain CPU conversion, NV12 to 0xAABBGGRR with BT.709 limited-range
 * coefficients and nearest-neighbour scaling. It is the first, simple way to
 * get a picture on screen; the intended one hands the decoder's frames to the
 * GPU without copying.
 */
#include "mirror_view.hpp"

#include "player.hpp"
#include "vdec.hpp"

#include <atomic>
#include <cstddef>
#include <ctime>

namespace mirage
{
namespace
{
constexpr unsigned kMaxCanvasWidth = 3840;
constexpr std::uint32_t kBlack = 0xff000000u;

// Per-value terms of the conversion, scaled by 256, built once.
int luma_term[256];
int red_from_v[256], green_from_u[256], green_from_v[256], blue_from_u[256];
bool tables_built = false;

std::atomic<unsigned long> drawn{0}, last_serial{0};
std::atomic<unsigned> last_draw_ms{0}, worst_draw_ms{0}, shown_width{0}, shown_height{0};
std::atomic<bool> synced{false};
constexpr std::int64_t kNsPerSecond = 1000000000;

// Picks the picture for this frame. With sound playing, the one that belongs
// to what is being heard now; otherwise simply the newest.
bool lock_picture_for_now(Picture &picture) noexcept
{
    std::int64_t offset = 0;
    if (audio_schedule_offset(offset))
    {
        timespec now{};
        (void)clock_gettime(CLOCK_MONOTONIC, &now); // the clock the player's schedule is on
        const std::int64_t due = static_cast<std::int64_t>(now.tv_sec) * kNsPerSecond + now.tv_nsec - offset;
        if (!vdec_lock_picture(picture, due))
            return false;
        // A picture seconds away from "due" means the sound and the pictures
        // are not on one clock after all: do not hold the screen for that.
        if (picture.time_ns - due < 2 * kNsPerSecond && due - picture.time_ns < 2 * kNsPerSecond)
        {
            synced.store(true);
            return true;
        }
        vdec_unlock_picture();
    }
    synced.store(false);
    return vdec_lock_picture(picture, INT64_MAX);
}

void build_tables() noexcept
{
    for (int value = 0; value < 256; ++value)
    {
        luma_term[value] = 298 * (value - 16) + 128;
        red_from_v[value] = 459 * (value - 128);
        green_from_u[value] = -55 * (value - 128);
        green_from_v[value] = -136 * (value - 128);
        blue_from_u[value] = 541 * (value - 128);
    }
    tables_built = true;
}

inline std::uint32_t clamp8(int scaled) noexcept
{
    const int value = scaled >> 8;
    return static_cast<std::uint32_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

unsigned elapsed_ms(const timespec &from) noexcept
{
    timespec now{};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<unsigned>((now.tv_sec - from.tv_sec) * 1000 + (now.tv_nsec - from.tv_nsec) / 1000000);
}

void fill(std::uint32_t *pixels, std::size_t count) noexcept
{
    for (std::size_t i = 0; i < count; ++i)
        pixels[i] = kBlack;
}
} // namespace

bool draw_mirror(RowWriter write, void *context, unsigned canvas_width, unsigned canvas_height,
                 unsigned visible_width, unsigned visible_height) noexcept
{
    static unsigned source_column[kMaxCanvasWidth];
    static std::uint32_t row[kMaxCanvasWidth], black_row[kMaxCanvasWidth];
    Picture picture{};
    timespec started{};

    if (!write || canvas_width == 0 || canvas_height == 0 || canvas_width > kMaxCanvasWidth ||
        !lock_picture_for_now(picture))
        return false;
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    if (!tables_built)
        build_tables();

    // The part of the picture that is meant to be seen.
    unsigned width = picture.width, height = picture.height;
    if (visible_width >= 2 && visible_width < width)
        width = visible_width & ~1u;
    if (visible_height >= 2 && visible_height < height)
        height = visible_height & ~1u;

    // Largest size with the picture's shape that fits the frame.
    unsigned out_width = canvas_width;
    unsigned out_height = static_cast<unsigned>(static_cast<std::uint64_t>(canvas_width) * height / width);
    if (out_height > canvas_height)
    {
        out_height = canvas_height;
        out_width = static_cast<unsigned>(static_cast<std::uint64_t>(canvas_height) * width / height);
    }
    if (out_width == 0 || out_height == 0)
    {
        vdec_unlock_picture();
        return false;
    }
    const unsigned left = (canvas_width - out_width) / 2;
    const unsigned top = (canvas_height - out_height) / 2;
    for (unsigned x = 0; x < out_width; ++x)
        source_column[x] = static_cast<unsigned>(static_cast<std::uint64_t>(x) * width / out_width);

    fill(black_row, canvas_width);
    fill(row, canvas_width); // the bars beside the picture stay black in every row
    for (unsigned y = 0; y < top; ++y)
        write(context, y, black_row);
    for (unsigned y = 0; y < out_height; ++y)
    {
        const unsigned source_row = static_cast<unsigned>(static_cast<std::uint64_t>(y) * height / out_height);
        const std::uint8_t *luma = picture.luma + static_cast<std::size_t>(source_row) * picture.width;
        const std::uint8_t *chroma = picture.chroma + static_cast<std::size_t>(source_row / 2) * picture.width;
        std::uint32_t *out = row + left;
        for (unsigned x = 0; x < out_width; ++x)
        {
            const unsigned column = source_column[x];
            const int l = luma_term[luma[column]];
            const unsigned u = chroma[column & ~1u], v = chroma[(column & ~1u) + 1];
            out[x] = kBlack | clamp8(l + blue_from_u[u]) << 16 | clamp8(l + green_from_u[u] + green_from_v[v]) << 8 |
                     clamp8(l + red_from_v[v]);
        }
        write(context, top + y, row);
    }
    for (unsigned y = top + out_height; y < canvas_height; ++y)
        write(context, y, black_row);

    const unsigned long serial = picture.serial;
    vdec_unlock_picture();

    const unsigned took = elapsed_ms(started);
    last_draw_ms.store(took);
    if (took > worst_draw_ms.load())
        worst_draw_ms.store(took);
    drawn.fetch_add(1);
    last_serial.store(serial);
    shown_width.store(out_width);
    shown_height.store(out_height);
    return true;
}

MirrorViewStatus mirror_view_status() noexcept
{
    MirrorViewStatus status{};
    status.drawn = drawn.load();
    status.serial = last_serial.load();
    status.last_draw_ms = last_draw_ms.load();
    status.worst_draw_ms = worst_draw_ms.load();
    status.shown_width = shown_width.load();
    status.shown_height = shown_height.load();
    status.synced = synced.load();
    return status;
}
} // namespace mirage
