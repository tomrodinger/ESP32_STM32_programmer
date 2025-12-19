#include <cstdio>

#include "swd_min.h"

#include "sim_api.h"

int main() {
  // Write this standalone sim into its own CSV in the repo root.
  sim::set_log_path("read_simulation.csv");

  // Configure pins to match ESP32 project defaults.
  static const swd_min::Pins pins(35, 36, 37);
  swd_min::begin(pins);

  sim::log_step("STEP_READ_BEGIN");
  swd_min::reset_and_switch_to_swd();

  sim::log_step("STEP_READ_IDCODE_REQ");
  uint8_t ack = 0;
  uint32_t idcode = 0;
  const bool ok = swd_min::read_idcode(&idcode, &ack);
  std::printf("IDCODE: ack=%u ok=%d idcode=0x%08X\n", ack, ok ? 1 : 0, idcode);
  sim::log_step(ok ? "STEP_READ_IDCODE_OK" : "STEP_READ_IDCODE_FAIL");

  std::printf(
      "DEBUG flags: swdio_input_pullup_seen=%d target_drove_swdio_seen=%d target_voltage_logged_seen=%d contention_seen=%d\n",
      sim::swdio_input_pullup_seen() ? 1 : 0,
      sim::target_drove_swdio_seen() ? 1 : 0,
      sim::target_voltage_logged_seen() ? 1 : 0,
      sim::contention_seen() ? 1 : 0);

  if (sim::contention_seen()) {
    std::printf("\n========================================\n");
    std::printf("WARNING: SWDIO contention detected (host+target both driving)\n");
    std::printf("Check SWDIO turnaround handling; log marks this as 1.65V\n");
    std::printf("========================================\n\n");
  }

  std::printf("Wrote log: read_simulation.csv\n");
  return ok ? 0 : 2;
}

