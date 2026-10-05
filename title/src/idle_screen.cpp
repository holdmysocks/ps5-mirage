/*
 * Mirage - title: the screen between sessions.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "idle_screen.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace mirage
{
namespace
{
// Text centred on the screen. A character is 5 dots wide and they are 6 apart.
void centred(ps5::demo::Canvas &canvas, unsigned y, std::string_view text, unsigned scale,
             ps5::demo::Color color) noexcept
{
    const unsigned width = text.empty() ? 0 : (static_cast<unsigned>(text.size()) * 6 - 1) * scale;
    const unsigned screen = ps5::demo::Canvas::width;
    canvas.text(width >= screen ? 0 : (screen - width) / 2, y, text, scale, color);
}


// part of 256 of `over`, the rest `under`; 0xAABBGGRR.
ps5::demo::Color mix(ps5::demo::Color over, ps5::demo::Color under, unsigned part) noexcept
{
    const auto a = static_cast<std::uint32_t>(over), b = static_cast<std::uint32_t>(under);
    std::uint32_t result = 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8)
        result |= ((((a >> shift) & 0xff) * part + ((b >> shift) & 0xff) * (256 - part) + 128) >> 8) << shift;
    return static_cast<ps5::demo::Color>(result);
}

// A triangle, point up, with smooth sloping sides: Canvas::triangle fills
// whole pixels, which shows as steps. The pixel at each end of a row is
// mixed with `behind` by how much of it the triangle covers. Only the rows
// from first_row up to last_row are drawn.
void smooth_triangle(ps5::demo::Canvas &canvas, unsigned center_x, unsigned top, unsigned half_width, unsigned height,
                     ps5::demo::Color color, ps5::demo::Color behind, unsigned first_row, unsigned last_row) noexcept
{
    for (unsigned row = first_row; row < last_row && row < height; ++row)
    {
        // Half the width at the middle of this row, in 256ths of a pixel.
        const unsigned exact = (2 * row + 1) * half_width * 256 / (2 * height);
        const unsigned whole = exact >> 8, part = exact & 0xff;
        canvas.rectangle(center_x - whole, top + row, 2 * whole + 1, 1, color);
        if (part != 0)
        {
            const ps5::demo::Color edge = mix(color, behind, part);
            canvas.rectangle(center_x - whole - 1, top + row, 1, 1, edge);
            canvas.rectangle(center_x + whole + 1, top + row, 1, 1, edge);
        }
    }
}
} // namespace

// The screen between sessions: what this is, the name to look for on the
// sending device, whether it is ready, and a pairing code when one is asked
// for, in the Dracula colours like the home screen tile and backgrounds
// (tools\make-art.py). The font has capitals, digits and . : - % / only.
void draw_idle(ps5::demo::Canvas &canvas, const PlayerStatus &status, const char *problem) noexcept
{
    using ps5::demo::Color;
    const unsigned middle = ps5::demo::Canvas::width / 2;

    canvas.clear(Color::dracula_background);

    // A screen with a triangle rising into it.
    canvas.rectangle(middle - 150, 110, 300, 190, Color::dracula_purple);
    canvas.rectangle(middle - 136, 124, 272, 162, Color::dracula_background);
    // The gap around the triangle only shows where it cuts the screen's
    // lower edge (rows 286 to 299): there the purple is behind it.
    smooth_triangle(canvas, middle, 226, 112, 98, Color::dracula_background, Color::dracula_purple, 286 - 226, 300 - 226);
    smooth_triangle(canvas, middle, 250, 80, 70, Color::dracula_pink, Color::dracula_background, 0, 70);

    centred(canvas, 380, "MIRAGE", 14, Color::dracula_foreground);

    std::array<char, 97> name{};
    std::snprintf(name.data(), name.size(), "%s", status.listed_name);
    for (char &c : name)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    const std::size_t name_length = std::strlen(name.data());
    if (name_length > 0)
    {
        // As large as fits, the longest name still on one line.
        unsigned scale = 1800 / (static_cast<unsigned>(name_length) * 6);
        scale = scale > 7 ? 7 : (scale < 2 ? 2 : scale);
        centred(canvas, 540, "THIS CONSOLE IS LISTED AS", 3, Color::dracula_comment);
        centred(canvas, 585, std::string_view{name.data(), name_length}, scale, Color::dracula_cyan);
    }

    if (status.pin[0] != 0)
    {
        // The code, one digit at a time with room between them.
        const std::size_t digits = std::strlen(status.pin);
        constexpr unsigned kScale = 18, kStep = 9 * kScale;
        canvas.rectangle(middle - 420, 690, 840, 300, Color::dracula_current_line);
        centred(canvas, 715, "PAIRING CODE", 4, Color::dracula_comment);
        unsigned x = middle - (static_cast<unsigned>(digits) * kStep - 4 * kScale) / 2;
        for (std::size_t i = 0; i < digits; ++i, x += kStep)
            canvas.text(x, 770, std::string_view{&status.pin[i], 1}, kScale, Color::dracula_foreground);
        centred(canvas, 935, "ENTER IT ON THE DEVICE THAT ASKS FOR IT", 3, Color::dracula_comment);
    }
    else if (!status.threads_started)
    {
        centred(canvas, 720, "COULD NOT START", 5, Color::dracula_red);
    }
    else if (status.link_connected)
    {
        if (status.buffered_ms > 0)
        {
            centred(canvas, 720, "PLAYING AUDIO", 5, Color::dracula_pink);
        }
        else
        {
            centred(canvas, 720, "READY FOR AIRPLAY", 5, Color::dracula_green);
            centred(canvas, 795, "PICK THIS CONSOLE FROM THE AIRPLAY MENU ON YOUR IPHONE IPAD OR MAC", 3, Color::dracula_comment);
        }
    }
    else
    {
        centred(canvas, 720, "STARTING", 5, Color::dracula_yellow);
    }

    if (problem != nullptr && problem[0] != 0)
        centred(canvas, 1020, problem, 3, Color::dracula_red);
}
} // namespace mirage
