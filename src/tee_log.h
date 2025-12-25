#pragma once

#include <Arduino.h>

namespace tee_log {

// Initialize tee logging.
// Must be called after Serial.begin().
void begin();

// Output sink that writes to USB serial (Serial) AND the RAM log.
Print &out();

// Temporarily disable RAM capture while still writing to Serial.
// Useful for dumping the RAM log itself.
void set_capture_enabled(bool enabled);
bool capture_enabled();

class ScopedCaptureSuspend {
 public:
  ScopedCaptureSuspend();
  ~ScopedCaptureSuspend();

 private:
  bool prev_ = true;
};

}  // namespace tee_log

