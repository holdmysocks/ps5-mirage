/*
 * Mirage - title: the screen between sessions.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "demo_renderer.hpp"
#include "player.hpp"

namespace mirage
{
// Draws the whole screen: what this is, the name the console is listed under,
// whether it is ready, and the pairing code while status.pin holds one.
// problem: one line about what is wrong (capitals and digits), or null.
void draw_idle(ps5::demo::Canvas &canvas, const PlayerStatus &status, const char *problem) noexcept;
} // namespace mirage
