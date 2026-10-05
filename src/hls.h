/*
 * Mirage - video casting (AirPlay "play this URL", as the YouTube app does).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How a cast works with UxPlay's library: the app asks the receiver to play
 * an HLS stream it names; the library has the *app* fetch the playlists (the
 * receiver may not be able to), rewrites them, serves them from its own HTTP
 * server and then calls on_video_play with a local URL of the master
 * playlist. A player is expected to open that URL, fetch the media segments
 * itself from the addresses in the playlists, and play them.
 *
 * Step H1 (this file so far): no player. The module records what the app
 * asks for, fetches the local master playlist to show that the library's
 * side works on the console, and answers the app's questions about playback
 * with a clock that simply runs, so the session stays up.
 */
#ifndef MIRAGE_HLS_H
#define MIRAGE_HLS_H

#include "pcm_server.h"
#include "raop.h"

void hls_init(pcm_server_log_t log);

/* UxPlay's callbacks, passed straight through. */
void hls_on_play(const char *location, float start_position);
void hls_on_scrub(float position);
void hls_on_rate(float rate);
void hls_on_stop(void);
float hls_on_playlist_remove(void);
void hls_on_playback_info(playback_info_t *info);

/* The cast is over (the library reset its video state). */
void hls_reset(void);
/* Whether a cast is in progress. */
int hls_active(void);

#endif
