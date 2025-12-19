#include <cstdio>

#include "stm32g0_prog.h"
#include "swd_min.h"

#include "sim_api.h"

// Use a tiny payload to keep simulation logs/waveforms small.
static const uint8_t firmware_bin_8[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
static const unsigned int firmware_bin_8_len = sizeof(firmware_bin_8);

static bool run_all() {
  // First: just prove SWD link with a DP IDCODE read.
  // (This is equivalent to your serial command 'i', and it generates an easy-to-recognize waveform.)
  {
    sim::log_step("STEP_IDCODE_BEGIN");
    uint8_t ack = 0;
    uint32_t idcode = 0;

    swd_min::reset_and_switch_to_swd();
    const bool ok = swd_min::read_idcode(&idcode, &ack);

    std::printf("IDCODE: ack=%u ok=%d idcode=0x%08X\n", ack, ok ? 1 : 0, idcode);
    sim::log_step(ok ? "STEP_IDCODE_OK" : "STEP_IDCODE_FAIL");
  }

  // Then: full programming flow.
  sim::log_step("STEP_CONNECT_BEGIN");
  if (!stm32g0_prog::connect_and_halt()) {
    std::printf("connect_and_halt failed\n");
    sim::log_step("STEP_CONNECT_FAIL");
    return false;
  }
  sim::log_step("STEP_CONNECT_OK");

  sim::log_step("STEP_ERASE_BEGIN");
  if (!stm32g0_prog::flash_mass_erase()) {
    std::printf("flash_mass_erase failed\n");
    sim::log_step("STEP_ERASE_FAIL");
    return false;
  }
  sim::log_step("STEP_ERASE_OK");

  sim::log_step("STEP_PROGRAM_BEGIN");
  if (!stm32g0_prog::flash_program(stm32g0_prog::FLASH_BASE, firmware_bin_8, firmware_bin_8_len)) {
    std::printf("flash_program failed\n");
    sim::log_step("STEP_PROGRAM_FAIL");
    return false;
  }
  sim::log_step("STEP_PROGRAM_OK");

  sim::log_step("STEP_VERIFY_BEGIN");
  const bool ok = stm32g0_prog::flash_verify_and_dump(stm32g0_prog::FLASH_BASE, firmware_bin_8, firmware_bin_8_len);
  if (!ok) std::printf("verify failed\n");
  sim::log_step(ok ? "STEP_VERIFY_OK" : "STEP_VERIFY_FAIL");
  return ok;
}

int main() {
  // Configure pins to match ESP32 project defaults.
  static const swd_min::Pins pins(35, 36, 37);

  swd_min::begin(pins);

  // Run the full programming flow (equivalent to serial command 'a').
  const bool ok = run_all();

  std::printf(
      "DEBUG flags: swdio_input_pullup_seen=%d target_drove_swdio_seen=%d target_voltage_logged_seen=%d contention_seen=%d\n",
      sim::swdio_input_pullup_seen() ? 1 : 0,
      sim::target_drove_swdio_seen() ? 1 : 0,
      sim::target_voltage_logged_seen() ? 1 : 0,
      sim::contention_seen() ? 1 : 0);

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
