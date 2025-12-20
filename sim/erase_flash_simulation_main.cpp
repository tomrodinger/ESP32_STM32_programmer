#include <cstdio>

#include "stm32g0_prog.h"
#include "swd_min.h"

#include "sim_api.h"

static void print_bytes(const uint8_t *buf, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    std::printf("%02X%s", buf[i], (i + 1 == n) ? "" : " ");
  }
}

int main() {
  // Write this standalone sim into its own CSV in the repo root.
  sim::set_log_path("erase_flash_simulation.csv");

  // Configure pins to match ESP32 project defaults.
  static const swd_min::Pins pins(35, 36, 37);
  swd_min::begin(pins);

  std::printf("erase_flash_simulation: starting\n");
  std::printf("Goal: perform STM32G031 mass erase via FLASH registers over SWD and verify by dumping first 32 bytes.\n\n");

  // Step 0: Attach + prove physical link with IDCODE.
  sim::log_step("STEP_0_ATTACH_BEGIN");
  std::printf("Step 0: Attach SWD (line reset + JTAG-to-SWD) while holding NRST low.\n");
  swd_min::reset_and_switch_to_swd();

  sim::log_step("STEP_0_IDCODE_READ");
  std::printf("Step 0: Read DP.IDCODE to confirm the link is alive.\n");
  uint8_t ack = 0;
  uint32_t idcode = 0;
  const bool id_ok = swd_min::read_idcode(&idcode, &ack);
  std::printf("  Result: ack=%u ok=%d idcode=0x%08X\n\n", ack, id_ok ? 1 : 0, idcode);

  // Step 1: Erase using the production-intended firmware function.
  sim::log_step("STEP_1_ERASE_BEGIN");
  std::printf("Step 1: Connect+halt and perform mass erase using the FLASH_ERASE.md sequence.\n\n");

  const bool ok = stm32g0_prog::connect_and_halt() && stm32g0_prog::flash_mass_erase();
  sim::log_step(ok ? "STEP_1_ERASE_OK" : "STEP_1_ERASE_FAIL");

  // Step 2: Read back first 32 bytes and print.
  sim::log_step("STEP_2_DUMP_BEGIN");
  uint8_t buf[32] = {0};
  uint32_t optr = 0;
  const bool read_ok = stm32g0_prog::flash_read_bytes(/*addr=*/stm32g0_prog::FLASH_BASE, buf, /*len=*/32, &optr);
  sim::log_step(read_ok ? "STEP_2_DUMP_OK" : "STEP_2_DUMP_FAIL");

  if (ok && read_ok) {
    std::printf("Erase OK.\n");
    std::printf("FLASH_OPTR @ 0x40022020 = 0x%08X (RDP byte=0x%02X)\n", optr, (unsigned)(optr & 0xFFu));
    std::printf("Flash[0x08000000..0x0800001F] = ");
    print_bytes(buf, 32);
    std::printf("\n");

    bool erased = true;
    for (uint32_t i = 0; i < 32; i++) {
      if (buf[i] != 0xFFu) {
        erased = false;
        break;
      }
    }
    std::printf("Erased check (first 32 bytes all 0xFF): %s\n", erased ? "PASS" : "FAIL");
  } else {
    std::printf("Erase or readback failed.\n");
  }

  std::printf(
      "\nDEBUG flags: swdio_input_pullup_seen=%d target_drove_swdio_seen=%d target_voltage_logged_seen=%d contention_seen=%d\n",
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

  std::printf("Wrote log: erase_flash_simulation.csv\n");
  return (ok && read_ok) ? 0 : 2;
}

