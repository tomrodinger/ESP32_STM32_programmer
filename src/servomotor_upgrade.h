#pragma once

#include <Arduino.h>

// Forward declaration to avoid pulling in the whole Arduino library header from users.
class Servomotor;

namespace servomotor_upgrade {

// Upgrade the motor controller main firmware over RS485 using the Servomotor
// Arduino library.
//
// The firmware file is expected to be in SPIFFS at `firmware_path` and to be
// in the `.firmware` format used by `upgrade_firmware.py`.
//
// Returns true on success.
bool upgrade_main_firmware_by_unique_id(Servomotor &motor, const char *firmware_path);

// Convenience/debug helper: query product info over RS485 and print it.
// Returns true only if a valid response was received.
bool print_product_info_by_unique_id(Servomotor &motor);

}  // namespace servomotor_upgrade
