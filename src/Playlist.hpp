#pragma once

#include <Arduino.h>

// Playlists: a text file in /matrix/<board>/playlist/*.txt listing GIFs,
// images and Lua scripts, each with how long it plays (format:
// spectre_protocol docs/playlist.md). The matrix plays it by itself.
namespace Playlist {

// Read and start a playlist (what played before is stopped by the caller).
// 0 or a spectre::error code (NotFound, BadArgs: no playable line).
uint16_t start(const char* path);
// Stop it (what it shows stays on screen).
void stop();
bool active();
const char* path();
// Next (1) / previous (-1) item. False when no playlist plays.
bool skip(int delta);
// Show the current item again (the display was rebuilt).
void replay();
// Advance when the current item is done. From loop().
void loop();

}  // namespace Playlist
