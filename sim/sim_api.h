#pragma once

namespace sim {

// Simulator-only helpers exposed for the host executable.
bool contention_seen();
bool swdio_input_pullup_seen();
bool target_drove_swdio_seen();
bool target_voltage_logged_seen();

// Log a point-event into signals.csv at the current simulated time.
// Intended for high-level step markers (shown in the waveform viewer).
// Example name: "STEP_IDCODE_BEGIN".
void log_step(const char *name);

// Must be called before any Arduino shim function (pinMode/digitalWrite/...) is used.
// Sets the CSV output path for the current simulation executable.
void set_log_path(const char *path);

} // namespace sim
