#pragma once

namespace sim {

// Simulator-only helpers exposed for the host executable.
bool contention_seen();
bool swdio_input_pullup_seen();
bool target_drove_swdio_seen();
bool target_voltage_logged_seen();

} // namespace sim
