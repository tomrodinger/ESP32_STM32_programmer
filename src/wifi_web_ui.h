#pragma once

#include <Arduino.h>

namespace wifi_web_ui {

// Starts a FreeRTOS task pinned to the other core that runs:
// - WiFi softAP
// - HTTP server with status + serial endpoints
void start_task();

}  // namespace wifi_web_ui

