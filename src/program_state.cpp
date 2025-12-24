#include "program_state.h"

namespace program_state {

static SemaphoreHandle_t g_mu = nullptr;
static String g_fw;

static SemaphoreHandle_t mu() {
  if (!g_mu) g_mu = xSemaphoreCreateMutex();
  return g_mu;
}

void set_firmware_filename(const String &path) {
  xSemaphoreTake(mu(), portMAX_DELAY);
  g_fw = path;
  xSemaphoreGive(mu());
}

String firmware_filename() {
  xSemaphoreTake(mu(), portMAX_DELAY);
  const String out = g_fw;
  xSemaphoreGive(mu());
  return out;
}

}  // namespace program_state

