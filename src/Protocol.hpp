#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "Log.hpp"
#include <stdint.h>

// Spectre Protocol over BLE (../spectre_protocol): the only way to control
// the matrix. Handlers run in loop(), from protocol_loop().
void protocol_begin(NimBLEServer* server);
void protocol_loop();
void protocol_disconnected();

// Send a sys.log event to connected clients. Safe from any task (queued,
// sent from protocol_loop); dropped if the queue is full.
void protocol_log(LogLevel level, const char* text);
// A Spectre Protocol client is talking over Serial right now.
bool protocol_serial_active();

// Board name: this project's files live in /matrix/<board>/{gif,png,lua}, so
// one SD card can serve several projects. Saved in Preferences ("board").
// board_begin() loads it and creates the folders (Preferences and the
// filesystem must be ready).
void board_begin();
// "/matrix/<board>" or "/matrix/<board>/<sub>"
String board_dir(const char* sub = nullptr);

// Play a stored file by extension (.gif / .png / .lua).
// Returns 0 or a spectre::error code.
uint16_t play_file(const char* path);
