#include <Arduino.h>

#include "binary.h"
#include "stm32g0_prog.h"
#include "swd_min.h"

static const swd_min::Pins PINS(35, 36, 37);

static void print_user_pressed_banner(char c) {
  Serial.printf("=== User pressed %c ========================\n", c);
}

static void print_help() {
  Serial.println("Commands:");
  Serial.println("  h = help");
  Serial.println("  i = reset + read DP IDCODE");
  Serial.println("  R = let firmware run: clear debug-halt state, pulse NRST, then release SWD pins");
  Serial.println("  t = SWD smoke test (DP power-up handshake + AHB-AP IDR)");
  Serial.println("  d = toggle SWD verbose diagnostics");
  Serial.println("  b = DP ABORT write test (write under NRST low, then under NRST high)");
  Serial.println("  c = DP CTRL/STAT single-write test (DP[0x04]=0x50000000)");
  Serial.println("  p = read Program Counter (PC) register (tests core register access)");
  Serial.println("  r = read first 8 bytes of target flash @ 0x08000000");
  Serial.println("  e = erase entire flash (mass erase; connect-under-reset recovery method)");
  Serial.println("  w = write firmware to flash");
  Serial.println("      (prints a simple benchmark: connect/program/total time)");
  Serial.println("  v = verify firmware in flash (dumps bytes read + mismatch count)");
  Serial.println("  a = all: connect+halt, erase, write, verify");
}

static void cmd_reset_pulse_run() {
  // A reliable way to "let the core run again" after it has been halted by SWD:
  // 1) Clear any debug-state that can keep the CPU halted (DHCSR.C_HALT)
  //    and disable vector-catch-on-reset (DEMCR.VC_CORERESET).
  // 2) Pulse NRST low, then release.
  //
  // Note: if the target firmware re-purposes SWD pins, a physically connected debugger
  // (or our own SWD line driving) can interfere electrically. We therefore avoid doing
  // any SWD activity after the reset pulse.

  Serial.println("Preparing target for normal run (clear C_HALT + clear VC_CORERESET)...");
  const bool prep_ok = stm32g0_prog::prepare_target_for_normal_run();
  Serial.println(prep_ok ? "Prep OK" : "Prep FAIL (continuing with NRST pulse)");

  Serial.println("Pulsing NRST LOW for 2ms, then releasing HIGH...");
  swd_min::set_nrst(true);
  delay(2);
  swd_min::set_nrst(false);

  // After reset, avoid continuing to drive SWD lines; target firmware may repurpose them.
  swd_min::release_swd_pins();
}

static bool print_idcode_attempt() {
  uint8_t ack = 0;
  uint32_t idcode = 0;

  // If verbose is enabled, the attach helper prints the attach banner lines.
  const bool ok = swd_min::attach_and_read_idcode(&idcode, &ack);

  Serial.printf("SWD ACK: %u (%s)\n", ack, swd_min::ack_to_str(ack));
  if (ok) {
    Serial.printf("DP IDCODE: 0x%08lX\n", (unsigned long)idcode);
  } else {
    Serial.println("DP IDCODE read failed.");
  }
  return ok;
}

static bool cmd_connect() {
  Serial.println("Connecting + halting core...");
  const bool ok = stm32g0_prog::connect_and_halt();
  Serial.println(ok ? "OK" : "FAIL");
  return ok;
}

static bool cmd_erase() {
  // Historical note: 'e' originally used a normal connect+halt flow. We now route
  // mass erase through the connect-under-reset recovery flow so it also works on
  // chips where user firmware disables SWD quickly.
  const bool ok = stm32g0_prog::flash_mass_erase_under_reset();
  Serial.println(ok ? "Erase OK" : "Erase FAIL");
  return ok;
}

static bool cmd_write() {
  // Benchmark 'w' without changing SWD clock:
  // - keep existing verbose setting for connect reliability
  // - disable verbose only during programming (Serial prints dominate runtime)
  // - measure connect + program + total
  const bool prev_verbose = swd_min::verbose_enabled();

  const uint32_t t0 = millis();
  const bool connect_ok = stm32g0_prog::connect_and_halt();
  const uint32_t t1 = millis();

  bool prog_ok = false;
  if (connect_ok) {
    swd_min::set_verbose(false);
    prog_ok = stm32g0_prog::flash_program(stm32g0_prog::FLASH_BASE, firmware_bin, firmware_bin_len);
    swd_min::set_verbose(prev_verbose);
  }
  const uint32_t t2 = millis();

  const uint32_t ms_connect = t1 - t0;
  const uint32_t ms_program = t2 - t1;
  const uint32_t ms_total = t2 - t0;

  // Throughput estimate (payload bytes / programming time). Avoid div by zero.
  const float prog_s = (ms_program > 0) ? (ms_program / 1000.0f) : 0.0001f;
  const float kbps = (firmware_bin_len / 1024.0f) / prog_s;

  Serial.printf("Benchmark w: connect=%lums program=%lums total=%lums (%.2f KiB/s over program phase)\n",
                (unsigned long)ms_connect, (unsigned long)ms_program, (unsigned long)ms_total, (double)kbps);

  const bool ok = connect_ok && prog_ok;
  Serial.println(ok ? "Write OK" : "Write FAIL");
  return ok;
}

static bool cmd_verify() {
  if (!cmd_connect()) return false;
  const bool ok = stm32g0_prog::flash_verify_and_dump(stm32g0_prog::FLASH_BASE, firmware_bin, firmware_bin_len);
  Serial.println(ok ? "Verify OK (all bytes match)" : "Verify FAIL (see mismatch count above)");
  return ok;
}

static bool cmd_read_flash_first_8() {
  Serial.println("Reading first 8 bytes of target flash via SWD...");

  // Make this command self-contained: it should work on a fresh/unprogrammed chip
  // without requiring the user to run 'i' first.
  if (!cmd_connect()) {
    Serial.println("Read FAIL (could not connect + halt)");
    return false;
  }

  uint8_t buf[8] = {0};
  uint32_t optr = 0;
  const bool ok = stm32g0_prog::flash_read_bytes(/*addr=*/stm32g0_prog::FLASH_BASE, buf, /*len=*/8, &optr);

  if (!ok) {
    Serial.println("Read FAIL");
    return false;
  }

  Serial.printf("FLASH_OPTR @ 0x40022020 = 0x%08lX (RDP byte=0x%02X)\n", (unsigned long)optr,
                (unsigned)((optr >> 0) & 0xFFu));

  Serial.printf("0x%08lX: ", (unsigned long)stm32g0_prog::FLASH_BASE);
  for (int i = 0; i < 8; i++) {
    Serial.printf("%02X ", buf[i]);
  }
  Serial.println();
  return true;
}

static bool cmd_swd_smoke_test() {
  Serial.println("SWD smoke test...");
  // Assumes SWD already attached via 'i' or the boot-time auto attempt.
  uint8_t ack = 0;

  // 2) DP power-up handshake
  Serial.println("DP init + power-up...");
  const bool dp_ok = swd_min::dp_init_and_power_up();
  Serial.println(dp_ok ? "DP init OK" : "DP init FAIL");
  if (!dp_ok) return false;

  // 3) Release reset then read AHB-AP IDR (tests DP.SELECT + AP read semantics)
  swd_min::set_nrst(false);
  delay(5);

  Serial.println("Reading AHB-AP IDR (AP register 0xFC)...");
  uint32_t ap_idr = 0;
  const bool ap_ok = swd_min::ap_read_reg(swd_min::AP_ADDR_IDR, &ap_idr, &ack);
  Serial.printf("AP IDR: ok=%d ack=%u (%s) idr=0x%08lX\n", ap_ok ? 1 : 0, (unsigned)ack, swd_min::ack_to_str(ack),
                (unsigned long)ap_idr);
  return ap_ok;
}

static bool cmd_toggle_verbose() {
  const bool enabled = !swd_min::verbose_enabled();
  swd_min::set_verbose(enabled);
  Serial.printf("SWD verbose: %s\n", enabled ? "ON" : "OFF");
  return true;
}

static bool cmd_dp_abort_write_test() {
  // The bench failure shows ACK=7 (invalid) for the first DP write (ABORT clear).
  // This test runs the exact same DP write twice:
  //   1) while NRST is still asserted low (as left by reset_and_switch_to_swd)
  //   2) after releasing NRST high
  // so you can compare scope waveforms and see if NRST state is the deciding factor.
  const uint32_t abort_clear = (1u << 4) | (1u << 3) | (1u << 2) | (1u << 1); // 0x1E

  Serial.println("DP ABORT write test (no reset)...");
  Serial.println("Phase 1: DP WRITE ABORT=0x0000001E");
  {
    uint8_t ack = 0;
    const bool ok = swd_min::dp_write_reg(swd_min::DP_ADDR_ABORT, abort_clear, &ack);
    Serial.printf("ABORT write: ok=%d ack=%u (%s)\n", ok ? 1 : 0, (unsigned)ack, swd_min::ack_to_str(ack));
  }

  Serial.println("Phase 2: release NRST HIGH, delay 5ms, then DP WRITE ABORT=0x0000001E");
  swd_min::set_nrst(false);
  delay(5);
  {
    uint8_t ack = 0;
    const bool ok = swd_min::dp_write_reg(swd_min::DP_ADDR_ABORT, abort_clear, &ack);
    Serial.printf("ABORT write (NRST HIGH): ok=%d ack=%u (%s)\n", ok ? 1 : 0, (unsigned)ack, swd_min::ack_to_str(ack));
  }

  return true;
}

static bool cmd_ap_csw_write_readback_test() {
  // Goal: a *single* DP write immediately after attach:
  // DP CTRL/STAT (addr 0x04) = 0x50000000
  // This matches the ADIv5 power-up request pattern used by our bring-up code.
  static constexpr uint32_t CTRLSTAT_PWRUP_REQ = 0x50000000u; // (1<<30)|(1<<28)

  // Assumes SWD already attached via 'i' or the boot-time auto attempt.
  // (Bench observation: a prior IDCODE read makes the following DP write ACK reliably.)
  Serial.println("DP CTRL/STAT single-write test (no reset, no IDCODE read)...");

  uint8_t ack = 0;
  Serial.printf("Writing DP CTRL/STAT (DP 0x%02X) = 0x%08lX...\n", (unsigned)swd_min::DP_ADDR_CTRLSTAT,
                (unsigned long)CTRLSTAT_PWRUP_REQ);
  const bool ok = swd_min::dp_write_reg(swd_min::DP_ADDR_CTRLSTAT, CTRLSTAT_PWRUP_REQ, &ack);
  Serial.printf("DP WRITE CTRL/STAT: ok=%d ack=%u (%s)\n", ok ? 1 : 0, (unsigned)ack, swd_min::ack_to_str(ack));
  return ok;
}

static bool cmd_all() {
  if (!cmd_connect()) return false;
  if (!stm32g0_prog::flash_mass_erase()) return false;
  if (!stm32g0_prog::flash_program(stm32g0_prog::FLASH_BASE, firmware_bin, firmware_bin_len)) return false;
  return stm32g0_prog::flash_verify_and_dump(stm32g0_prog::FLASH_BASE, firmware_bin, firmware_bin_len);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("\nESP32-S3 STM32G0 Programmer");
  Serial.println("Wiring: GPIO35=SWCLK GPIO36=SWDIO GPIO37=NRST");
  Serial.printf("Embedded firmware size: %u bytes\n", firmware_bin_len);

  swd_min::begin(PINS);

  Serial.printf("SWD verbose: %s (default)\n", swd_min::verbose_enabled() ? "ON" : "OFF");
  Serial.printf("Initial NRST state (driven by ESP32): %s\n", swd_min::nrst_is_high() ? "HIGH" : "LOW");

  print_help();
  Serial.println();

  // Do one attempt automatically on boot.
  print_idcode_attempt();
}

void loop() {
  if (!Serial.available()) {
    delay(10);
    return;
  }

  const char c = (char)Serial.read();

  // Ignore whitespace/newlines from terminal.
  if (c == '\n' || c == '\r' || c == ' ') {
    return;
  }

  print_user_pressed_banner(c);

  switch (c) {
    case 'h':
    case '?':
      print_help();
      break;

    case 'i':
      print_idcode_attempt();
      break;

    case 'R':
      cmd_reset_pulse_run();
      break;

    case 't':
      cmd_swd_smoke_test();
      break;

    case 'd':
      cmd_toggle_verbose();
      break;

    case 'b':
      cmd_dp_abort_write_test();
      break;

    case 'c':
      cmd_ap_csw_write_readback_test();
      break;

    case 'p':
      Serial.println("Reading Program Counter...");
      {
        const bool ok = stm32g0_prog::read_program_counter();
        Serial.println(ok ? "PC read: SUCCESS" : "PC read: FAILED");
      }
      break;

    case 'r':
      cmd_read_flash_first_8();
      break;

    case 'e':
      cmd_erase();
      break;

    case 'w':
      cmd_write();
      break;

    case 'v':
      cmd_verify();
      break;

    case 'a':
      Serial.println("Running full programming sequence...");
      {
        const bool ok = cmd_all();
        Serial.println(ok ? "SUCCESS" : "FAILED");
      }
      break;

    default:
      Serial.printf("Unknown command '%c' (0x%02X). Press 'h' for help.\n", c, (unsigned)c);
      break;
  }
}
