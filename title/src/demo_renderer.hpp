/*
 * ps5-native-app-boilerplate - Small CPU-rendered demonstration API.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Keeps PS5 VideoOut setup and the bitmap font out of the starter main file.
 *
 * Mirage: copied from ps5-native-app-boilerplate (src/demo_renderer.*) with
 * one addition, Canvas::write_row() and the frame size, so that a decoded
 * picture can be written into the frame a row at a time.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace ps5::demo
{
enum class Color : std::uint32_t
{
    background = UINT32_C(0xff190d0a),
    panel = UINT32_C(0xff301f17),
    white = UINT32_C(0xffffffff),
    cyan = UINT32_C(0xffffff00),
    magenta = UINT32_C(0xffff00ff),
    yellow = UINT32_C(0xff00ffff),
    // The Dracula colours (draculatheme.com), for the idle screen; 0xAABBGGRR.
    dracula_background = UINT32_C(0xff362a28),
    dracula_current_line = UINT32_C(0xff5a4744),
    dracula_foreground = UINT32_C(0xfff2f8f8),
    dracula_comment = UINT32_C(0xffa47262),
    dracula_cyan = UINT32_C(0xfffde98b),
    dracula_green = UINT32_C(0xff7bfa50),
    dracula_orange = UINT32_C(0xff6cb8ff),
    dracula_pink = UINT32_C(0xffc679ff),
    dracula_purple = UINT32_C(0xfff993bd),
    dracula_red = UINT32_C(0xff5555ff),
    dracula_yellow = UINT32_C(0xff8cfaf1),
};

class Canvas;
using DrawScene = void (*)(Canvas &) noexcept;
// Draws the scene once and keeps it on screen.
[[noreturn]] void run(DrawScene draw, std::string_view ready_message) noexcept;
// Calls draw for every frame, for a scene that changes. The callback redraws the whole
// frame; drawing on the CPU, a full 1920x1080 frame takes longer than one display refresh.
[[noreturn]] void run_frames(DrawScene draw, std::string_view ready_message) noexcept;

namespace detail
{
[[noreturn]] void present(DrawScene draw, std::string_view ready_message,
                          bool every_frame) noexcept;
}

class Canvas final
{
  public:
    static constexpr unsigned width = 1920;
    static constexpr unsigned height = 1080;
    // Writes one whole row: `width` pixels of 0xAABBGGRR. The frame memory is
    // tiled (128 x 128 pixel blocks with the pixels interleaved inside), not
    // row after row, so a picture cannot simply be copied into it: writing it
    // as if it were linear scrambles it (seen on the console, 2026-10-05).
    void write_row(unsigned y, const std::uint32_t *row) noexcept;
    void clear(Color color) noexcept;
    void rectangle(unsigned x, unsigned y, unsigned width, unsigned height, Color color) noexcept;
    void circle(unsigned center_x, unsigned center_y, unsigned radius, Color color) noexcept;
    void triangle(unsigned center_x, unsigned top, unsigned half_width, unsigned height,
                  Color color) noexcept;
    void text(unsigned x, unsigned y, std::string_view value, unsigned scale, Color color) noexcept;

  private:
    explicit Canvas(std::uint32_t *pixels) noexcept : pixels_{pixels}
    {
    }

    std::uint32_t *pixels_;

    friend void detail::present(DrawScene draw, std::string_view ready_message,
                                bool every_frame) noexcept;
};

void read_asset_text(const char *path, std::span<char> destination,
                     std::string_view fallback) noexcept;
} // namespace ps5::demo
