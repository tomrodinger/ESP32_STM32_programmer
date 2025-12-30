#pragma once

#include <Arduino.h>

namespace program_state {

// Cached status for the web UI.
void set_firmware_filename(const String &path);
String firmware_filename();

// Cached servomotor main firmware selection (SM*). Used by Mode 2 and the web UI.
void set_servomotor_firmware_filename(const String &path);
String servomotor_firmware_filename();

}  // namespace program_state
