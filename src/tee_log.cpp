#include "tee_log.h"

#include "ram_log.h"

namespace tee_log {

static bool g_inited = false;
static bool g_capture_enabled = true;

class TeePrint final : public Print {
 public:
  size_t write(uint8_t b) override {
    const size_t w = Serial.write(b);
    if (g_capture_enabled) {
      ram_log::write(&b, 1);
    }
    return w;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    const size_t w = Serial.write(buffer, size);
    if (g_capture_enabled) {
      ram_log::write(buffer, size);
    }
    return w;
  }
};

static TeePrint g_out;

void begin() {
  if (g_inited) return;
  ram_log::begin();
  g_inited = true;
}

Print &out() {
  if (!g_inited) begin();
  return g_out;
}

void set_capture_enabled(bool enabled) { g_capture_enabled = enabled; }

bool capture_enabled() { return g_capture_enabled; }

ScopedCaptureSuspend::ScopedCaptureSuspend() {
  prev_ = g_capture_enabled;
  g_capture_enabled = false;
}

ScopedCaptureSuspend::~ScopedCaptureSuspend() { g_capture_enabled = prev_; }

}  // namespace tee_log

