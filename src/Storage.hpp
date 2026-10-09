#pragma once

#include <FS.h>

// Where the files live: the SD card when one answers, else the SPIFFS
// partition of the flash (boards without a card slot).
extern fs::FS* storage;
extern bool storage_is_sd;

// Mounts one of them; false: neither.
bool storage_begin();
uint64_t storage_total();
uint64_t storage_used();
