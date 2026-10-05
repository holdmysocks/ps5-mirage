/*
 * Mirage - title.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title is the half that can be heard: it starts the payload half
 * (mirage.elf, which runs the AirPlay protocol) and plays what that sends
 * over loopback. While nothing is mirrored or cast the screen says under
 * which name the console is listed and whether it is ready; a build with
 * MIRAGE_DEBUG_OVERLAY shows the link and playback figures instead, so that a
 * test can be read off the TV.
 */
#include "demo_renderer.hpp"
#include "idle_screen.hpp"
#include "mirror_view.hpp"
#include "player.hpp"
#include "vdec.hpp"
#include "video.hpp"

#include <array>
#include <cstdio>

namespace
{
void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    const mirage::PlayerStatus status = mirage::player_status();
    const mirage::VideoStatus video = mirage::video_status();

    // While a mirrored picture exists it takes the whole screen, with one
    // small line of figures over it for testing.
    const auto write_row = [](void *context, unsigned y, const std::uint32_t *row) {
        static_cast<ps5::demo::Canvas *>(context)->write_row(y, row);
    };
    if (mirage::draw_mirror(write_row, &canvas, ps5::demo::Canvas::width, ps5::demo::Canvas::height, video.width,
                            video.height))
    {
#ifdef MIRAGE_DEBUG_OVERLAY // tools\build-title.ps1 -DebugOverlay: one line of figures over the picture
        const mirage::MirrorViewStatus view = mirage::mirror_view_status();
        const mirage::VdecStatus decoded = mirage::vdec_status();
        std::array<char, 96> overlay{};
        std::snprintf(overlay.data(), overlay.size(), "PICTURES %lu  HELD %lu  REORD %lu  ERRORS %lu  LAST %08X  %u X %u  DRAW %u MS  FPS %u  %s",
                      decoded.pictures, decoded.held, decoded.reordered, decoded.errors,
                      static_cast<unsigned>(decoded.last_error), decoded.width, decoded.height, view.worst_draw_ms,
                      video.units_per_second, view.synced ? "SYNC" : "FREE");
        canvas.text(24, 24, overlay.data(), 2, ps5::demo::Color::yellow);
#endif
        return;
    }

#ifndef MIRAGE_DEBUG_OVERLAY
    (void)video;
    // What is wrong, if anything is.
    const mirage::VdecStatus vdec = mirage::vdec_status();
    std::array<char, 96> problem{};
    if (status.audio_error != 0)
        std::snprintf(problem.data(), problem.size(), "AUDIO FAILED %08X", static_cast<unsigned>(status.audio_error));
    else if (vdec.failed)
        std::snprintf(problem.data(), problem.size(), "VIDEO DECODER FAILED AT %s  %08X",
                      mirage::vdec_stage_name(vdec.stage), static_cast<unsigned>(vdec.result));
    else if (status.launch_error != 0)
        std::snprintf(problem.data(), problem.size(), "THE PAYLOAD DID NOT START  ERROR %d", status.launch_error);
    else if (!status.link_connected && status.connect_errno != 0 && status.launches > 0)
        std::snprintf(problem.data(), problem.size(), "WAITING FOR THE PAYLOAD  ERRNO %d", status.connect_errno);
    mirage::draw_idle(canvas, status, problem.data());
#else
    using ps5::demo::Color;
    const mirage::VdecStatus vdec = mirage::vdec_status();
    std::array<char, 96> link{}, audio{}, counters{}, payload{}, video_rate{}, video_totals{};
    std::array<char, 96> decoder{}, pictures{};
    Color link_color = Color::white;
    Color audio_color = Color::white;
    Color payload_color = Color::white;

    if (!status.threads_started)
    {
        std::snprintf(link.data(), link.size(), "THREADS DID NOT START");
        link_color = Color::magenta;
    }
    else if (status.link_connected)
    {
        std::snprintf(link.data(), link.size(), "READY  PICK MIRAGE ON YOUR DEVICE");
        link_color = Color::cyan;
    }
    else if (status.connect_errno != 0)
    {
        std::snprintf(link.data(), link.size(), "STARTING  ERRNO %d", status.connect_errno);
        link_color = Color::yellow;
    }
    else
    {
        std::snprintf(link.data(), link.size(), "STARTING");
    }

    if (status.audio_error != 0)
    {
        std::snprintf(audio.data(), audio.size(), "AUDIO FAILED %08X",
                      static_cast<unsigned>(status.audio_error));
        audio_color = Color::magenta;
    }
    else
    {
        std::snprintf(audio.data(), audio.size(), status.times_ignored ? "BUFFER %u MS  TIMES IGNORED" : "BUFFER %u MS",
                      status.buffered_ms);
        if (status.times_ignored)
            audio_color = Color::yellow;
    }
    std::snprintf(counters.data(), counters.size(), "RECEIVED %lu S  DRY %u  LATE %u  RESTART %u  RESCHED %u  BAD %lu",
                  status.received_seconds, status.underruns, status.late_drops, status.restarts, status.reschedules,
                  status.bad_messages);
    std::snprintf(payload.data(), payload.size(), "PAYLOAD STARTED %u  ERROR %d  CONNECTS %lu",
                  status.launches, status.launch_error, status.connects);
    if (status.launch_error != 0)
        payload_color = Color::magenta;

    canvas.clear(Color::background);
    canvas.text(120, 60, "MIRAGE", 10, Color::white);
    // The name this console is listed under on the sending device. The font has capitals and digits only.
    std::array<char, 96> listed{};
    if (status.listed_name[0] != 0)
        std::snprintf(listed.data(), listed.size(), "LISTED AS  %s", status.listed_name);
    else
        std::snprintf(listed.data(), listed.size(), "%s", status.link_connected ? "" : "STARTING");
    for (char &c : listed)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    canvas.text(700, 100, listed.data(), 4, Color::white);
    canvas.rectangle(120, 190, 1680, 8, Color::white);
    canvas.text(120, 240, link.data(), 6, link_color);
    canvas.text(120, 350, audio.data(), 6, audio_color);
    canvas.text(120, 440, counters.data(), 4, Color::white);
    canvas.text(120, 500, payload.data(), 4, payload_color);

    // The hardware decoder: how far its bring-up got, then what it produces.
    if (vdec.failed)
        std::snprintf(decoder.data(), decoder.size(), "DECODER FAILED AT %s  %08X",
                      mirage::vdec_stage_name(vdec.stage), static_cast<unsigned>(vdec.result));
    else
        std::snprintf(decoder.data(), decoder.size(), "DECODER %s", mirage::vdec_stage_name(vdec.stage));
    std::snprintf(pictures.data(), pictures.size(), "PICTURES %lu  NONE %lu  HELD %lu  REORD %lu  ERRORS %lu  DROPPED %lu  LAST %08X",
                  vdec.pictures, vdec.no_picture, vdec.held, vdec.reordered, vdec.errors, vdec.dropped,
                  static_cast<unsigned>(vdec.last_error));
    std::array<char, 96> picture_format{};
    std::snprintf(picture_format.data(), picture_format.size(), "%u X %u  PITCH %u  SEQ %u  NEW %u  RESET %08X  WORST %u MS  COPY %u MS",
                  vdec.width, vdec.height, vdec.pitch, vdec.sequence_changes, vdec.recreations,
                  static_cast<unsigned>(vdec.reset_result), vdec.worst_decode_ms, vdec.worst_copy_ms);
    canvas.rectangle(120, 570, 1680, 4, Color::white);
    canvas.text(120, 600, decoder.data(), 5,
                vdec.failed ? Color::magenta : (vdec.stage == mirage::VdecStage::ready ? Color::cyan : Color::yellow));
    canvas.text(120, 680, pictures.data(), 4, vdec.errors ? Color::magenta : Color::white);
    canvas.text(120, 740, picture_format.data(), 4, Color::white);

    // What the sender delivers.
    std::snprintf(video_rate.data(), video_rate.size(), "VIDEO %s  %u X %u  %u FPS  %u KBIT",
                  video.units == 0 ? "NONE" : (video.h265 ? "H265" : "H264"), video.width, video.height,
                  video.units_per_second, video.kbit_per_second);
    std::snprintf(video_totals.data(), video_totals.size(), "UNITS %lu  KEY %lu  SPS %lu  MAX %u KB  SPAN %ld S  RESET %u",
                  video.units, video.keyframes, video.parameter_sets, video.largest_unit / 1024,
                  static_cast<long>(video.time_span_ms / 1000), video.resets);
    canvas.rectangle(120, 830, 1680, 4, Color::white);
    canvas.text(120, 870, video_rate.data(), 5, video.units_per_second ? Color::cyan : Color::white);
    canvas.text(120, 950, video_totals.data(), 4, Color::white);
#endif
}
} // namespace

int main()
{
    (void)mirage::start_player();
    (void)mirage::vdec_start();
    ps5::demo::run_frames(draw_scene, "Mirage");
}
