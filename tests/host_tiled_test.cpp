/*
 * Mirage - host test of Canvas::write_row (title\src\demo_renderer.cpp).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The frame memory is tiled. write_row uses tables; the boilerplate's text
 * and rectangle code computes each pixel's place directly, and is known to
 * be right on the console. This writes a different colour to every pixel with
 * write_row, then checks single pixels against rectangles of one pixel drawn
 * the boilerplate's way: both must land on the same bytes.
 */
#include "demo_renderer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The renderer's console functions. They are never called by this test (it
// does not present anything); they only have to exist for the link.
extern "C"
{
    int sceKernelGetDirectMemorySize(...) { return 0; }
    int sceKernelAllocateDirectMemory(...) { return -1; }
    int sceKernelMapDirectMemory(...) { return -1; }
    int sceKernelSendNotificationRequest(...) { return 0; }
    int sceKernelUsleep(...) { return 0; }
    int sceSystemServiceHideSplashScreen(...) { return 0; }
    int sceVideoOutOpen(...) { return -1; }
    int sceVideoOutSetFlipRate(...) { return 0; }
    int sceVideoOutSubmitFlip(...) { return 0; }
    int sceVideoOutWaitVblank(...) { return 0; }
    void sceVideoOutSetBufferAttribute2(...) {}
    int sceVideoOutRegisterBuffers2(...) { return -1; }
    int ps5ObserveOwnedAllocation(...) { return 0; } // the boilerplate's runtime
}

int main()
{
    using ps5::demo::Canvas;
    using ps5::demo::Color;
    const std::size_t bytes = 0x1000000; // one frame, as the renderer allocates it
    auto *by_rows = static_cast<std::uint32_t *>(std::calloc(bytes, 1));
    auto *by_pixels = static_cast<std::uint32_t *>(std::calloc(bytes, 1));
    // Canvas has a private constructor; its only member is the pixel pointer.
    Canvas rows = *reinterpret_cast<Canvas *>(&by_rows);
    Canvas pixels = *reinterpret_cast<Canvas *>(&by_pixels);

    // 1. Every pixel gets a unique value through write_row; no two may land on the same place.
    static std::uint32_t row[Canvas::width];
    for (unsigned y = 0; y < Canvas::height; ++y)
    {
        for (unsigned x = 0; x < Canvas::width; ++x)
            row[x] = 0x01000000u + y * Canvas::width + x;
        rows.write_row(y, row);
    }
    std::size_t written = 0;
    for (std::size_t i = 0; i < bytes / 4; ++i)
        written += by_rows[i] != 0;
    const bool unique = written == static_cast<std::size_t>(Canvas::width) * Canvas::height;

    // 2. The same places as the boilerplate's own drawing: white rows through write_row
    //    against white rectangles, over a spread of rows, must give identical memory.
    std::memset(by_rows, 0, bytes);
    for (unsigned x = 0; x < Canvas::width; ++x)
        row[x] = static_cast<std::uint32_t>(Color::white);
    bool same = true;
    for (unsigned y : {0u, 1u, 7u, 8u, 127u, 128u, 129u, 500u, 1023u, 1024u, 1079u})
    {
        rows.write_row(y, row);
        pixels.rectangle(0, y, Canvas::width, 1, Color::white);
    }
    same = std::memcmp(by_rows, by_pixels, bytes) == 0;

    // 3. Single pixels: one coloured pixel in a black row.
    std::memset(by_rows, 0, bytes);
    std::memset(by_pixels, 0, bytes);
    bool single = true;
    for (unsigned y : {0u, 77u, 128u, 1079u})
        for (unsigned x : {0u, 1u, 127u, 128u, 961u, 1919u})
        {
            std::memset(row, 0, sizeof(row));
            row[x] = static_cast<std::uint32_t>(Color::cyan);
            std::memset(by_rows, 0, bytes);
            std::memset(by_pixels, 0, bytes);
            rows.write_row(y, row);
            pixels.rectangle(x, y, 1, 1, Color::cyan);
            single = single && std::memcmp(by_rows, by_pixels, bytes) == 0;
        }

    std::printf("tiled   unique places %d, rows match rectangles %d, single pixels match %d\n", unique, same, single);
    const bool ok = unique && same && single;
    std::puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
