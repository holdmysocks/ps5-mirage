/*
 * Mirage - title: starts the payload and plays the PCM it sends over loopback.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>

namespace mirage
{
struct PlayerStatus
{
    bool threads_started;
    bool link_connected;
    int audio_error;        // 0, or the failing sceAudioOutInit/Open result
    int connect_errno;      // errno of the last failed connect to the payload, 0 if none
    unsigned buffered_ms;
    unsigned underruns;     // times playback ran dry mid-stream
    unsigned late_drops;    // times overdue audio was skipped to keep the delay
    unsigned restarts;      // times the incoming times jumped (a new stream without a flush)
    bool times_ignored;     // the sender's times were not usable; playing in arrival order
    unsigned reschedules;   // times a live stream's schedule was pulled back to its baseline
    unsigned long received_seconds;
    unsigned long connects;
    unsigned long bad_messages;
    unsigned launches;      // times this title sent the payload to the loader
    int launch_error;       // errno of the last launch, 0 if it was sent whole
    char listed_name[97];   // the name the payload advertises under ("" until it says)
    char pin[9];            // the pairing code to show ("" for none)
};

// Starts the link thread and the audio thread. False if a thread did not start.
bool start_player() noexcept;
PlayerStatus player_status() noexcept;
// While sound is playing on a schedule: the offset that turns a sender time
// into the local CLOCK_MONOTONIC time at which that moment is heard. Pictures
// from the same sender are shown by it. False when nothing is scheduled.
bool audio_schedule_offset(std::int64_t &offset_ns) noexcept;
} // namespace mirage
