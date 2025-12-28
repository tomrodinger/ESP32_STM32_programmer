#pragma once

#include <Arduino.h>

// Forward declaration to avoid pulling in the whole Arduino library header from users.
class Servomotor;

namespace servomotor_upgrade {

// Upgrade the motor controller main firmware over RS485 using the Servomotor
// Arduino library.
//
// `motor` must already be configured for the correct RS485 UART and must have
// been set to use extended addressing via motor.useUniqueId(unique_id).
//
// The firmware file must be stored on the ESP32 fwfs filesystem (SPIFFS) in the
// `.firmware` format used by the host tool. The extension of tthe filename has been removed
// to keep the character count 31 or less.
//
// If `firmware_path` is nullptr, the implementation will auto-select exactly
// one `SM*` file from SPIFFS root.
//
// Returns true only if all pages are ACKed.
bool upgrade_main_firmware_by_unique_id(Servomotor &motor, uint64_t unique_id, const char *firmware_path = nullptr);

// Convenience/debug helper: query product info over RS485 and print it.
// Returns true only if a valid response was received.
bool print_product_info_by_unique_id(Servomotor &motor);

}  // namespace servomotor_upgrade
