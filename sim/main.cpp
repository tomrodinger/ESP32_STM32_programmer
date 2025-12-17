#include <cstdio>

#include "swd_min.h"

#include "sim_api.h"

int main() {
  // Configure pins to match ESP32 project defaults.
  static const swd_min::Pins pins(35, 36, 37);

  // Start SWD bitbang
  swd_min::begin(pins);
  swd_min::reset_and_switch_to_swd();

  uint8_t ack = 0;
  uint32_t idcode = 0;
  const bool ok = swd_min::read_idcode(&idcode, &ack);

  std::printf("SWD ACK: %u\n", ack);
  if (ok) {
    std::printf("DP IDCODE: 0x%08X\n", idcode);
  } else {
    std::printf("DP IDCODE read failed\n");
  }

  std::printf(
      "DEBUG flags: swdio_input_pullup_seen=%d target_drove_swdio_seen=%d target_voltage_logged_seen=%d contention_seen=%d\n",
      sim::swdio_input_pullup_seen() ? 1 : 0,
      sim::target_drove_swdio_seen() ? 1 : 0,
      sim::target_voltage_logged_seen() ? 1 : 0,
      sim::contention_seen() ? 1 : 0);

  // Quick sanity checks on host/target handoff
  if (!sim::swdio_input_pullup_seen()) {
    std::printf("\n========================================\n");
    std::printf("WARNING: host never set SWDIO to INPUT_PULLUP in this run\n");
    std::printf("This would prevent proper turnaround and target-drive voltages\n");
    std::printf("========================================\n\n");
  }

  if (!sim::target_drove_swdio_seen()) {
    std::printf("\n========================================\n");
    std::printf("WARNING: target never drove SWDIO in this run\n");
    std::printf("If IDCODE succeeded, logging of target-drive may be broken\n");
    std::printf("========================================\n\n");
  }

  if (!sim::target_voltage_logged_seen()) {
    std::printf("\n========================================\n");
    std::printf("WARNING: SWDIO never logged a target-drive voltage (0.1V or 3.2V)\n");
    std::printf("This suggests the simulator is not actually reflecting target drive in the waveform.\n");
    std::printf("========================================\n\n");
  }

  // End-of-run warning for SWDIO contention
  if (sim::contention_seen()) {
    std::printf("\n========================================\n");
    std::printf("WARNING: SWDIO contention detected (host+target both driving)\n");
    std::printf("Check SWDIO turnaround handling; log marks this as 1.65V\n");
    std::printf("========================================\n\n");
  }

  std::printf("Wrote log: signals.csv\n");
  return ok ? 0 : 2;
}
