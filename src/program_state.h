#pragma once

#include <Arduino.h>

namespace program_state {

// Cached status for the web UI.
void set_firmware_filename(const String &path);
String firmware_filename();

}  // namespace program_state

