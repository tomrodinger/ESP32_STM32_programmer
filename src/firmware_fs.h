#pragma once

#include <Arduino.h>

namespace firmware_fs {

// Mount LittleFS at base path "/littlefs" using partition label "fwfs".
// Returns true on success.
bool begin();

// Prints basic FS stats + lists all files in the root directory.
void print_status();

// Find exactly one firmware file matching the pattern "bootloader*.bin".
//
// Contract:
// - if exactly one match exists, writes its full path (including base path) to out_path and returns true
// - if zero matches or more than one match exist, prints an error and returns false
bool find_single_firmware_bin(String &out_path);

}  // namespace firmware_fs

