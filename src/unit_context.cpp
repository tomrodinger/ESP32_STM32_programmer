#include "unit_context.h"

namespace unit_context {

static SemaphoreHandle_t g_mu = nullptr;
static Context g_ctx;

static SemaphoreHandle_t mu() {
  if (!g_mu) g_mu = xSemaphoreCreateMutex();
  return g_mu;
}

void set(const Context &c) {
  xSemaphoreTake(mu(), portMAX_DELAY);
  g_ctx = c;
  xSemaphoreGive(mu());
}

Context get() {
  xSemaphoreTake(mu(), portMAX_DELAY);
  const Context out = g_ctx;
  xSemaphoreGive(mu());
  return out;
}

void clear() {
  Context c;
  c.valid = false;
  set(c);
}

}  // namespace unit_context

