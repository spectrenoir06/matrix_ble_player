#pragma once

#include <NimBLEDevice.h>
#include <stdint.h>

// Spectre Protocol over BLE (../spectre_protocol): the only way to control
// the matrix. Handlers run in loop(), from protocol_loop().
void protocol_begin(NimBLEServer* server);
void protocol_loop();
void protocol_disconnected();

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warning = 2, Error = 3 };
// Send a sys.log event to connected clients. Safe from any task (queued,
// sent from protocol_loop); dropped if the queue is full.
void protocol_log(LogLevel level, const char* text);

// Play a stored file by extension (.gif / .png / .lua).
// Returns 0 or a spectre::error code.
uint16_t play_file(const char* path);
