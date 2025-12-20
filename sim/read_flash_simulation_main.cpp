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
  sim::set_log_path("read_flash_simulation.csv");

  // Configure pins to match ESP32 project defaults.
  static const swd_min::Pins pins(35, 36, 37);
  swd_min::begin(pins);

  std::printf("read_flash_simulation: starting\n");
  std::printf("Goal: read the first 8 bytes of STM32G031 flash @ 0x08000000 via SWD/AHB-AP and print them.\n\n");

  // STEP 0: Prove the physical/protocol layer with an IDCODE read.
  sim::log_step("STEP_0_ATTACH_BEGIN");
  std::printf("Step 0: Attach SWD (line reset + JTAG-to-SWD sequence) while holding NRST low.\n");
  swd_min::reset_and_switch_to_swd();

  sim::log_step("STEP_0_IDCODE_READ");
  std::printf("Step 0: Read DP.IDCODE to confirm the link is alive.\n");
  uint8_t ack = 0;
  uint32_t idcode = 0;
  const bool id_ok = swd_min::read_idcode(&idcode, &ack);
  std::printf("  Result: ack=%u ok=%d idcode=0x%08X\n\n", ack, id_ok ? 1 : 0, idcode);

  // STEP 1..4: Use the firmware-side helper which follows READ_FLASH.md.
  sim::log_step("STEP_1_READ_FLASH_BEGIN");
  std::printf("Step 1: Perform the recommended flash-read sequence from READ_FLASH.md:\n");
  std::printf("  1) DP init + power-up (CTRL/STAT handshake)\n");
  std::printf("  2) Release NRST high (memory access under reset is not guaranteed)\n");
  std::printf("  3) Short delay for clocks/bus fabric to come up\n");
  std::printf("  4) Halt the core (DHCSR)\n");
  std::printf("  5) Read flash bytes using AHB-AP memory reads\n\n");

  uint8_t buf[8] = {0};
  uint32_t optr = 0;
  const bool ok = stm32g0_prog::flash_read_bytes(/*addr=*/stm32g0_prog::FLASH_BASE, buf, /*len=*/8, &optr);

  sim::log_step(ok ? "STEP_1_READ_FLASH_OK" : "STEP_1_READ_FLASH_FAIL");

  if (ok) {
    std::printf("Read OK.\n");
    std::printf("FLASH_OPTR @ 0x40022020 = 0x%08X (RDP byte=0x%02X)\n", optr, (unsigned)(optr & 0xFFu));
    std::printf("Flash[0x08000000..0x08000007] = ");
    print_bytes(buf, 8);
    std::printf("\n");
  } else {
    std::printf("Read FAIL.\n");
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

  std::printf("Wrote log: read_flash_simulation.csv\n");
  return ok ? 0 : 2;
}

