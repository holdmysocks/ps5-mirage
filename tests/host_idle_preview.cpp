/*
 * Mirage - host preview of the title's idle screen (title\src\idle_screen.cpp).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Draws the screen for a few states into memory laid out like the console's
 * frame memory and writes each as a PPM picture, so that the layout can be
 * looked at without a console. Run by tools\preview-idle.ps1.
 */
#include "demo_renderer.hpp"
#include "idle_screen.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// The renderer's console functions. They are never called here (nothing is
// presented); they only have to exist for the link.
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

namespace
{
using ps5::demo::Canvas;
constexpr std::size_t kPixels = static_cast<std::size_t>(Canvas::width) * Canvas::height;
// The frame memory is tiled and larger than width x height; twice is plenty.
constexpr std::size_t kFrame = kPixels * 2;

// Canvas has a private constructor; its only member is the pixel pointer.
Canvas canvas_over(std::uint32_t *&pixels)
{
    return *reinterpret_cast<Canvas *>(&pixels);
}

bool write_ppm(const char *path, const std::vector<std::uint32_t> &frame, const std::vector<std::uint32_t> &where)
{
    // where[i]: 1 + the linear position of the pixel stored at frame index i, 0 for none.
    std::vector<std::uint8_t> rgb(kPixels * 3, 0);
    for (std::size_t i = 0; i < kFrame; ++i)
    {
        if (where[i] == 0)
            continue;
        const std::size_t at = (where[i] - 1) * 3;
        rgb[at] = static_cast<std::uint8_t>(frame[i]);           // 0xAABBGGRR
        rgb[at + 1] = static_cast<std::uint8_t>(frame[i] >> 8);
        rgb[at + 2] = static_cast<std::uint8_t>(frame[i] >> 16);
    }
    std::FILE *file = std::fopen(path, "wb");
    if (!file)
        return false;
    std::fprintf(file, "P6\n%u %u\n255\n", Canvas::width, Canvas::height);
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), file) == rgb.size();
    std::fclose(file);
    return ok;
}
} // namespace

int main()
{
    // Find out where each pixel lives: write every row with numbered pixels.
    std::vector<std::uint32_t> where(kFrame, 0), frame(kFrame, 0);
    {
        std::uint32_t *pixels = where.data();
        Canvas canvas = canvas_over(pixels);
        std::vector<std::uint32_t> row(Canvas::width);
        for (unsigned y = 0; y < Canvas::height; ++y)
        {
            for (unsigned x = 0; x < Canvas::width; ++x)
                row[x] = y * Canvas::width + x + 1;
            canvas.write_row(y, row.data());
        }
    }

    struct Case
    {
        const char *file;
        bool connected;
        unsigned buffered_ms;
        const char *name;
        const char *pin;
        const char *problem;
    };
    const Case cases[] = {
        {"idle-ready.ppm", true, 0, "Mirage - Living Room PS5", "", nullptr},
        {"idle-pin.ppm", true, 0, "Mirage - Living Room PS5", "4821", nullptr},
        {"idle-audio.ppm", true, 300, "Mirage - Living Room", "", nullptr},
        {"idle-starting.ppm", false, 0, "", "", "WAITING FOR THE PAYLOAD  ERRNO 61"},
        {"idle-long-name.ppm", true, 0, "Mirage - A console with a remarkably long name in the settings", "", nullptr},
    };
    for (const Case &c : cases)
    {
        mirage::PlayerStatus status{};
        status.threads_started = true;
        status.link_connected = c.connected;
        status.buffered_ms = c.buffered_ms;
        std::snprintf(status.listed_name, sizeof(status.listed_name), "%s", c.name);
        std::snprintf(status.pin, sizeof(status.pin), "%s", c.pin);
        std::uint32_t *pixels = frame.data();
        Canvas canvas = canvas_over(pixels);
        mirage::draw_idle(canvas, status, c.problem);
        if (!write_ppm(c.file, frame, where))
        {
            std::printf("could not write %s\n", c.file);
            return 1;
        }
        std::printf("wrote %s\n", c.file);
    }
    return 0;
}
