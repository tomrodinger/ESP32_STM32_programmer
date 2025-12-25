#include <Arduino.h>

#include <WiFi.h>

#include "firmware_fs.h"
#include "firmware_source.h"
#include "firmware_source_file.h"
#include "stm32g0_prog.h"
#include "swd_min.h"

#include <SPIFFS.h>

#include "product_info_injector_reader.h"
#include "program_state.h"
#include "serial_log.h"
#include "wifi_web_ui.h"

#include "first_block_override_reader.h"

#include "product_info.h"

static bool g_first_block_snapshot_valid = false;
static uint8_t g_first_block_snapshot[256];

static void set_first_block_snapshot(const uint8_t *b0, uint32_t n) {
  if (!b0 || n == 0) {
    g_first_block_snapshot_valid = false;
    return;
  }
  const uint32_t take = (n > sizeof(g_first_block_snapshot)) ? (uint32_t)sizeof(g_first_block_snapshot) : n;
  memcpy(g_first_block_snapshot, b0, take);
  if (take < sizeof(g_first_block_snapshot)) {
    memset(g_first_block_snapshot + take, 0xFF, sizeof(g_first_block_snapshot) - take);
  }
  g_first_block_snapshot_valid = true;
}

static const swd_min::Pins PINS(35, 36, 37);

// Production jig button:
// - GPIO45 configured as INPUT_PULLUP
// - external button pulls to GND when pressed
static constexpr int k_prod_button_pin = 45;
static constexpr uint32_t k_button_debounce_ms = 30;

// Forward declarations (used by production sequence helper).
static bool print_idcode_attempt();
static bool cmd_erase();
static bool cmd_write();
static bool cmd_verify();

static bool cmd_write_with_product_info(uint32_t serial, uint64_t unique_id);
static bool cmd_verify_with_product_info(uint32_t serial, uint64_t unique_id);

static void print_hex_dump_16(uint32_t base_addr, const uint8_t *data, uint32_t len);
static void print_product_info_struct(const product_info_struct &pi);
static bool cmd_consume_serial_record_only();
static bool cmd_print_logs();
static void cmd_print_wifi_ap_status();

static void print_user_pressed_banner(char c) {
  if (c == ' ') {
    Serial.println("=== User pressed <space> (0x20) ========================");
    return;
  }
  // Print both the character (when printable) and the raw byte value for debugging.
  if (c >= 32 && c <= 126) {
    Serial.printf("=== User pressed '%c' (0x%02X) ========================\n", c, (unsigned char)c);
  } else {
    Serial.printf("=== User pressed 0x%02X ========================\n", (unsigned char)c);
  }
}

static void print_help() {
  Serial.println("Commands:");
  Serial.println("  h = help");
  Serial.println("  f = filesystem status (SPIFFS) + list files");
  Serial.println("  F = select firmware file (must match BL*; exactly one match required)");
  Serial.println("  i = reset + read DP IDCODE");
  Serial.println("  s = consume a serial and append it to consumed-serial record (test only)");
  Serial.println("  S<serial> = set next serial (append USERSET_<serial>) (example: S1000)");
  Serial.println("  l = print logs to Serial (/log.txt + consumed serial record; prints last 50 records each)");
  Serial.println("  R = let firmware run: clear debug-halt state, pulse NRST, then release SWD pins");
  Serial.println("  t = SWD smoke test (DP power-up handshake + AHB-AP IDR)");
  Serial.println("  d = toggle SWD verbose diagnostics");
  Serial.println("  b = DP ABORT write test (write under NRST low, then under NRST high)");
  Serial.println("  c = DP CTRL/STAT single-write test (DP[0x04]=0x50000000)");
  Serial.println("  p = read Program Counter (PC) register (tests core register access)");
  Serial.println("  r = read first 8 bytes of target flash @ 0x08000000");
  Serial.println("  e = erase entire flash (mass erase; connect-under-reset recovery method)");
  Serial.println("  w = write firmware to flash (prints serial+unique_id, first block hexdump, product_info_struct)");
  Serial.println("      (prints a simple benchmark: connect/program/total time)");
  Serial.println("  v = verify firmware in flash (FAST; prints benchmark + mismatch count)");
  Serial.println("  a = access point (WiFi) status: up/down + IP address");
  Serial.println("  <space> = PRODUCTION: run i -> e -> w -> v -> R (fail-fast; stops at first error)");
  Serial.println("Production jig:");
  Serial.printf("  Button on GPIO%d (INPUT_PULLUP) pulls to GND when pressed -> runs <space> sequence\n",
                 k_prod_button_pin);
}

static bool ensure_fs_mounted() {
  static bool mounted = false;
  if (mounted) return true;

  Serial.println("Mounting SPIFFS (partition label fwfs, base path /spiffs)...");
  mounted = firmware_fs::begin();
  Serial.println(mounted ? "SPIFFS mount OK" : "SPIFFS mount FAIL");
  return mounted;
}

static bool select_firmware_path(String &out_path) {
  if (!ensure_fs_mounted()) return false;
  const bool ok = firmware_fs::find_single_firmware_bin(out_path);
  if (ok) program_state::set_firmware_filename(out_path);
  return ok;
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

static bool cmd_reset_pulse_run_strict() {
  // Strict variant intended for production sequence:
  // if the SWD prep fails, treat it as a hard failure and do not proceed.
  Serial.println("Preparing target for normal run (clear C_HALT + clear VC_CORERESET)...");
  const bool prep_ok = stm32g0_prog::prepare_target_for_normal_run();
  if (!prep_ok) {
    Serial.println("ERROR: Prep for run failed; not pulsing NRST");
    return false;
  }

  Serial.println("Pulsing NRST LOW for 2ms, then releasing HIGH...");
  swd_min::set_nrst(true);
  delay(2);
  swd_min::set_nrst(false);

  // After reset, avoid continuing to drive SWD lines; target firmware may repurpose them.
  swd_min::release_swd_pins();
  return true;
}

static bool run_production_sequence(const char *source) {
  Serial.println("========================================");
  Serial.printf("PRODUCTION sequence triggered by %s\n", source);
  Serial.println("Sequence: i -> e -> w -> v -> R (fail-fast)");
  Serial.println("----------------------------------------");

  // Fail-safe: do not program if filesystem has almost no free space.
  // This is required so we don't start a unit and then fail to persist logs.
  const size_t fs_total = (size_t)SPIFFS.totalBytes();
  const size_t fs_used = (size_t)SPIFFS.usedBytes();
  const size_t fs_free = (fs_total > fs_used) ? (fs_total - fs_used) : 0u;
  if (fs_free < 100u) {
    Serial.printf("ERROR: Production disabled: filesystem free space too low (%lu bytes)\n", (unsigned long)fs_free);
    return false;
  }

  if (!serial_log::has_serial_next()) {
    Serial.println("ERROR: Production disabled: next serial not set (use WiFi UI to set it)");
    return false;
  }

  String fw_path;
  if (!select_firmware_path(fw_path)) {
    Serial.println("ERROR: Production disabled: no valid firmware file selected");
    return false;
  }

  // Track successful steps for summary log.
  String completed_steps = "";
  serial_log::Consumed consumed;

  if (!print_idcode_attempt()) {
    Serial.println("ERROR: Production sequence aborted at step 'i' (IDCODE)");
    return false;
  }
  completed_steps += 'i';

  if (!cmd_erase()) {
    Serial.println("ERROR: Production sequence aborted at step 'e' (erase)");
    return false;
  }
  completed_steps += 'e';

  // Consume serial at the beginning of the 'w' phase.
  consumed = serial_log::consume_for_write();
  if (!consumed.valid) {
    Serial.println("ERROR: Serial consumption failed; aborting");
    return false;
  }

  Serial.printf("Production consumed serial=%lu unique_id=0x%08lX%08lX\n", (unsigned long)consumed.serial,
                (unsigned long)(consumed.unique_id >> 32), (unsigned long)(consumed.unique_id & 0xFFFFFFFFu));

  if (!cmd_write_with_product_info(consumed.serial, consumed.unique_id)) {
    Serial.println("ERROR: Production sequence aborted at step 'w' (write)");
    (void)serial_log::append_summary_with_unique_id(completed_steps.c_str(), consumed.serial, consumed.unique_id, /*ok=*/false);
    return false;
  }
  completed_steps += 'w';

  if (!cmd_verify_with_product_info(consumed.serial, consumed.unique_id)) {
    Serial.println("ERROR: Production sequence aborted at step 'v' (verify)");
    (void)serial_log::append_summary_with_unique_id(completed_steps.c_str(), consumed.serial, consumed.unique_id, /*ok=*/false);
    return false;
  }
  completed_steps += 'v';

  if (!cmd_reset_pulse_run_strict()) {
    Serial.println("ERROR: Production sequence aborted at step 'R' (run)");
    (void)serial_log::append_summary_with_unique_id(completed_steps.c_str(), consumed.serial, consumed.unique_id, /*ok=*/false);
    return false;
  }
  completed_steps += 'R';

  (void)serial_log::append_summary_with_unique_id(completed_steps.c_str(), consumed.serial, consumed.unique_id, /*ok=*/true);
  Serial.println("PRODUCTION sequence SUCCESS");
  return true;
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
  String fw_path;
  if (!select_firmware_path(fw_path)) {
    Serial.println("Write FAIL (no valid firmware file selected)");
    return false;
  }

  // For manual 'w' testing, consume a serial immediately and print it.
  if (!serial_log::has_serial_next()) {
    Serial.println("Write FAIL (serial not set; use WiFi UI or 's' command)");
    return false;
  }
  const serial_log::Consumed reserved = serial_log::consume_for_write();
  if (!reserved.valid) {
    Serial.println("Write FAIL (failed to reserve serial)");
    return false;
  }

  Serial.printf("Write will use serial=%lu unique_id=0x%08lX%08lX\n", (unsigned long)reserved.serial,
                (unsigned long)(reserved.unique_id >> 32), (unsigned long)(reserved.unique_id & 0xFFFFFFFFu));

  // Benchmark 'w' without changing SWD clock:
  // - keep existing verbose setting for connect reliability
  // - disable verbose only during programming (Serial prints dominate runtime)
  // - measure connect + program + total
  const bool prev_verbose = swd_min::verbose_enabled();

  firmware_source::FileReader file_reader(SPIFFS);
  if (!file_reader.open(fw_path.c_str())) {
    Serial.printf("Write FAIL (could not open firmware file: %s)\n", fw_path.c_str());
    return false;
  }
  firmware_source::ProductInfoInjectorReader injected(file_reader, reserved.serial, reserved.unique_id);
  firmware_source::Stm32G0Adapter fw_reader(injected);

  const uint32_t t0 = millis();
  const bool connect_ok = stm32g0_prog::connect_and_halt();
  const uint32_t t1 = millis();

  bool prog_ok = false;
  if (connect_ok) {
    swd_min::set_verbose(false);

    // Force first block load + patch, then print debug info.
    uint8_t tmp[1];
    uint32_t out_n = 0;
    (void)injected.read_at(0, tmp, 1, &out_n);
    const uint8_t *b0 = injected.first_block_ptr();
    if (b0) {
      set_first_block_snapshot(b0, firmware_source::ProductInfoInjectorReader::first_block_size());
      Serial.println("First 256 bytes to be programmed (after injection):");
      print_hex_dump_16(stm32g0_prog::FLASH_BASE, b0, firmware_source::ProductInfoInjectorReader::first_block_size());

      const uint32_t off = (uint32_t)(PRODUCT_INFO_MEMORY_LOCATION - stm32g0_prog::FLASH_BASE);
      product_info_struct pi;
      memcpy(&pi, b0 + off, sizeof(pi));
      print_product_info_struct(pi);
    }

    prog_ok = stm32g0_prog::flash_program_reader(stm32g0_prog::FLASH_BASE, fw_reader);
    swd_min::set_verbose(prev_verbose);
  }
  const uint32_t t2 = millis();

  const uint32_t ms_connect = t1 - t0;
  const uint32_t ms_program = t2 - t1;
  const uint32_t ms_total = t2 - t0;

  // Throughput estimate (payload bytes / programming time). Avoid div by zero.
  const float prog_s = (ms_program > 0) ? (ms_program / 1000.0f) : 0.0001f;
  const float kbps = (file_reader.size() / 1024.0f) / prog_s;

  Serial.printf("Benchmark w: connect=%lums program=%lums total=%lums (%.2f KiB/s over program phase)\n",
                (unsigned long)ms_connect, (unsigned long)ms_program, (unsigned long)ms_total, (double)kbps);

  const bool ok = connect_ok && prog_ok;
  Serial.println(ok ? "Write OK" : "Write FAIL");
  return ok;
}

static void print_hex_dump_16(uint32_t base_addr, const uint8_t *data, uint32_t len) {
  if (!data) return;
  for (uint32_t i = 0; i < len; i += 16) {
    Serial.printf("0x%08lX: ", (unsigned long)(base_addr + i));
    const uint32_t n = (len - i >= 16) ? 16 : (len - i);
    for (uint32_t j = 0; j < n; j++) {
      Serial.printf("%02X ", (unsigned)data[i + j]);
    }
    Serial.println();
  }
}

static void print_product_info_struct(const product_info_struct &pi) {
  Serial.println("product_info_struct:");
  char model[MODEL_CODE_LENGTH + 1];
  memcpy(model, pi.model_code, MODEL_CODE_LENGTH);
  model[MODEL_CODE_LENGTH] = 0;
  Serial.printf("  model_code: '%s'\n", model);
  Serial.printf("  firmware_compatibility_code: %u\n", (unsigned)pi.firmware_compatibility_code);
  Serial.printf("  hardware_version: %u.%u.%u\n", (unsigned)pi.hardware_version_major, (unsigned)pi.hardware_version_minor,
                (unsigned)pi.hardware_version_bugfix);
  Serial.printf("  serial_number: %lu\n", (unsigned long)pi.serial_number);
  Serial.printf("  unique_id: 0x%08lX%08lX\n", (unsigned long)(pi.unique_id >> 32),
                (unsigned long)(pi.unique_id & 0xFFFFFFFFu));
}

static bool cmd_write_with_product_info(uint32_t serial, uint64_t unique_id) {
  String fw_path;
  if (!select_firmware_path(fw_path)) {
    Serial.println("Write FAIL (no valid firmware file selected)");
    return false;
  }

  const bool prev_verbose = swd_min::verbose_enabled();

  firmware_source::FileReader file_reader(SPIFFS);
  if (!file_reader.open(fw_path.c_str())) {
    Serial.printf("Write FAIL (could not open firmware file: %s)\n", fw_path.c_str());
    return false;
  }

  Serial.printf("Write(prod) using serial=%lu unique_id=0x%08lX%08lX\n", (unsigned long)serial,
                (unsigned long)(unique_id >> 32), (unsigned long)(unique_id & 0xFFFFFFFFu));

  firmware_source::ProductInfoInjectorReader injected(file_reader, serial, unique_id);
  firmware_source::Stm32G0Adapter fw_reader(injected);

  const uint32_t t0 = millis();
  const bool connect_ok = stm32g0_prog::connect_and_halt();
  const uint32_t t1 = millis();

  bool prog_ok = false;
  if (connect_ok) {
    swd_min::set_verbose(false);

    // Ensure first block is materialized so we can snapshot it for subsequent verify.
    uint8_t tmp[1];
    uint32_t out_n = 0;
    (void)injected.read_at(0, tmp, 1, &out_n);
    const uint8_t *b0 = injected.first_block_ptr();
    if (b0) {
      set_first_block_snapshot(b0, firmware_source::ProductInfoInjectorReader::first_block_size());
    }

    prog_ok = stm32g0_prog::flash_program_reader(stm32g0_prog::FLASH_BASE, fw_reader);
    swd_min::set_verbose(prev_verbose);
  }
  const uint32_t t2 = millis();

  const uint32_t ms_connect = t1 - t0;
  const uint32_t ms_program = t2 - t1;
  const uint32_t ms_total = t2 - t0;

  const float prog_s = (ms_program > 0) ? (ms_program / 1000.0f) : 0.0001f;
  const float kbps = (file_reader.size() / 1024.0f) / prog_s;

  Serial.printf("Benchmark w(prod): connect=%lums program=%lums total=%lums (%.2f KiB/s)\n",
                (unsigned long)ms_connect, (unsigned long)ms_program, (unsigned long)ms_total, (double)kbps);

  const bool ok = connect_ok && prog_ok;
  Serial.println(ok ? "Write OK" : "Write FAIL");
  return ok;
}

static bool cmd_verify() {
  String fw_path;
  if (!select_firmware_path(fw_path)) {
    Serial.println("Verify FAIL (no valid firmware file selected)");
    return false;
  }

  firmware_source::FileReader file_reader(SPIFFS);
  if (!file_reader.open(fw_path.c_str())) {
    Serial.printf("Verify FAIL (could not open firmware file: %s)\n", fw_path.c_str());
    return false;
  }

  // Verify policy:
  // - If we have a snapshot of the injected first block, use it for offsets < 256.
  // - Otherwise verify the raw file for all bytes.
  firmware_source::Stm32G0Adapter fw_reader(file_reader);
  firmware_source::FirstBlockOverrideReader override0(file_reader,
                                                      g_first_block_snapshot_valid ? g_first_block_snapshot : nullptr,
                                                      g_first_block_snapshot_valid ? 256u : 0u);
  firmware_source::Stm32G0Adapter fw_reader_override(override0);

  // Production-oriented verify:
  // - Use aggressive connect-under-reset + immediate halt so user firmware cannot
  //   disable SWD pins before we halt the core.
  // - Use fast verify (bulk AHB-AP reads) and avoid dumping all bytes.
  const bool prev_verbose = swd_min::verbose_enabled();
  // For production speed, keep SWD verbose off for the whole verify operation.
  swd_min::set_verbose(false);

  const uint32_t t0 = millis();
  const bool connect_ok = stm32g0_prog::connect_and_halt_under_reset_recovery();
  const uint32_t t1 = millis();

  uint32_t mismatches = 0;
  bool verify_ok = false;
  if (connect_ok) {
    stm32g0_prog::FirmwareReader &r = g_first_block_snapshot_valid
                                         ? static_cast<stm32g0_prog::FirmwareReader &>(fw_reader_override)
                                         : static_cast<stm32g0_prog::FirmwareReader &>(fw_reader);
    verify_ok = stm32g0_prog::flash_verify_fast_reader(stm32g0_prog::FLASH_BASE, r, &mismatches, /*max_report=*/8);
  }
  const uint32_t t2 = millis();

  swd_min::set_verbose(prev_verbose);

  const uint32_t ms_connect = t1 - t0;
  const uint32_t ms_verify = t2 - t1;
  const uint32_t ms_total = t2 - t0;

  // Throughput estimate (payload bytes / verify time). Avoid div by zero.
  const float verify_s = (ms_verify > 0) ? (ms_verify / 1000.0f) : 0.0001f;
  const float kbps = (file_reader.size() / 1024.0f) / verify_s;

  Serial.printf("Benchmark v: connect=%lums verify=%lums total=%lums (%.2f KiB/s over verify phase)\n",
                (unsigned long)ms_connect, (unsigned long)ms_verify, (unsigned long)ms_total, (double)kbps);
  Serial.printf("Verify mismatches: %lu\n", (unsigned long)mismatches);

  const bool ok = connect_ok && verify_ok;
  Serial.println(ok ? "Verify OK (all bytes match)" : "Verify FAIL");
  return ok;
}

static bool cmd_verify_with_product_info(uint32_t serial, uint64_t unique_id) {
  String fw_path;
  if (!select_firmware_path(fw_path)) {
    Serial.println("Verify FAIL (no valid firmware file selected)");
    return false;
  }

  firmware_source::FileReader file_reader(SPIFFS);
  if (!file_reader.open(fw_path.c_str())) {
    Serial.printf("Verify FAIL (could not open firmware file: %s)\n", fw_path.c_str());
    return false;
  }

  Serial.printf("Verify(prod) expecting serial=%lu unique_id=0x%08lX%08lX\n", (unsigned long)serial,
                (unsigned long)(unique_id >> 32), (unsigned long)(unique_id & 0xFFFFFFFFu));

  // Prefer verifying against the first-block snapshot created during the write.
  // If snapshot is missing, fall back to re-injecting for verify.
  firmware_source::FirstBlockOverrideReader override0(file_reader,
                                                      g_first_block_snapshot_valid ? g_first_block_snapshot : nullptr,
                                                      g_first_block_snapshot_valid ? 256u : 0u);

  firmware_source::ProductInfoInjectorReader injected(file_reader, serial, unique_id);

  firmware_source::Stm32G0Adapter fw_reader_snapshot(override0);
  firmware_source::Stm32G0Adapter fw_reader_injected(injected);

  const bool prev_verbose = swd_min::verbose_enabled();
  swd_min::set_verbose(false);

  const uint32_t t0 = millis();
  const bool connect_ok = stm32g0_prog::connect_and_halt_under_reset_recovery();
  const uint32_t t1 = millis();

  uint32_t mismatches = 0;
  bool verify_ok = false;
  if (connect_ok) {
    stm32g0_prog::FirmwareReader &r = g_first_block_snapshot_valid
                                         ? static_cast<stm32g0_prog::FirmwareReader &>(fw_reader_snapshot)
                                         : static_cast<stm32g0_prog::FirmwareReader &>(fw_reader_injected);
    verify_ok = stm32g0_prog::flash_verify_fast_reader(stm32g0_prog::FLASH_BASE, r, &mismatches, /*max_report=*/8);
  }
  const uint32_t t2 = millis();

  swd_min::set_verbose(prev_verbose);

  const uint32_t ms_connect = t1 - t0;
  const uint32_t ms_verify = t2 - t1;
  const uint32_t ms_total = t2 - t0;

  const float verify_s = (ms_verify > 0) ? (ms_verify / 1000.0f) : 0.0001f;
  const float kbps = (file_reader.size() / 1024.0f) / verify_s;

  Serial.printf("Benchmark v(prod): connect=%lums verify=%lums total=%lums (%.2f KiB/s)\n",
                (unsigned long)ms_connect, (unsigned long)ms_verify, (unsigned long)ms_total, (double)kbps);
  Serial.printf("Verify mismatches: %lu\n", (unsigned long)mismatches);

  const bool ok = connect_ok && verify_ok;
  Serial.println(ok ? "Verify OK (all bytes match)" : "Verify FAIL");
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

void setup() {
  Serial.begin(115200);
  // IMPORTANT: do NOT block forever waiting for a USB serial monitor.
  //
  // On ESP32-S3 with native USB CDC, `while (!Serial)` can block indefinitely
  // when powered from a wall adapter (no host connected). That prevents:
  // - WiFi AP from starting
  // - the production button loop from running
  //
  // Allow a short grace period for developers (so early boot logs still show up
  // when a monitor is opened quickly), then continue headless.
  const uint32_t serial_wait_start_ms = millis();
  while (!Serial && (uint32_t)(millis() - serial_wait_start_ms) < 1500u) {
    delay(10);
  }

  Serial.println("\nESP32-S3 STM32G0 Programmer");
  Serial.println("Wiring: GPIO35=SWCLK GPIO36=SWDIO GPIO37=NRST");
  if (ensure_fs_mounted()) {
    firmware_fs::print_status();

    if (!serial_log::begin(SPIFFS)) {
      Serial.printf("Serial log init FAIL (%s)\n", serial_log::log_path());
    }
    if (serial_log::has_serial_next()) {
      Serial.printf("Next serial (loaded): %lu\n", (unsigned long)serial_log::serial_next());
    } else {
      Serial.println("Next serial (loaded): NOT SET (use WiFi UI to set it)");
    }

    String fw_path;
    if (firmware_fs::find_single_firmware_bin(fw_path)) {
      program_state::set_firmware_filename(fw_path);
      File f = SPIFFS.open(fw_path.c_str(), "r");
      if (f) {
        Serial.printf("Selected firmware size: %lu bytes\n", (unsigned long)f.size());
        f.close();
      }
    }
  }

  swd_min::begin(PINS);

  pinMode(k_prod_button_pin, INPUT_PULLUP);

  // Start WiFi AP + web UI on the other core.
  wifi_web_ui::start_task();

  Serial.printf("SWD verbose: %s (default)\n", swd_min::verbose_enabled() ? "ON" : "OFF");
  Serial.printf("Initial NRST state (driven by ESP32): %s\n", swd_min::nrst_is_high() ? "HIGH" : "LOW");

  print_help();
  Serial.println();

  // Do one attempt automatically on boot.
  print_idcode_attempt();
}

static bool cmd_consume_serial_record_only() {
  if (!ensure_fs_mounted()) return false;
  if (!serial_log::has_serial_next()) {
    Serial.println("Consume serial FAIL (serial not set; use WiFi UI)");
    return false;
  }
  const serial_log::Consumed c = serial_log::consume_for_write();
  if (!c.valid) {
    Serial.println("Consume serial FAIL (could not append to consumed record)");
    return false;
  }
  Serial.printf("Consume serial OK: consumed=%lu next=%lu\n", (unsigned long)c.serial,
                (unsigned long)serial_log::serial_next());
  return true;
}

static bool print_text_file_tail_lines(const char *path, uint32_t max_lines) {
  if (!path) return false;
  if (!ensure_fs_mounted()) return false;

  File f = SPIFFS.open(path, "r");
  if (!f) {
    Serial.printf("%s open FAIL (missing?)\n", path);
    return false;
  }

  // Keep last N lines in a ring buffer so we can print an omission header.
  String ring[50];
  const uint32_t cap = (max_lines > 50u) ? 50u : max_lines;
  uint32_t total = 0;
  uint32_t stored = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    ring[total % cap] = line;
    total++;
    if (stored < cap) stored++;
  }
  f.close();

  Serial.printf("--- %s ---\n", path);
  if (total > cap) {
    const uint32_t omitted = total - cap;
    Serial.printf("Log file too long. omitting preceding %lu records.\n", (unsigned long)omitted);
  }

  const uint32_t start = (total > cap) ? (total - cap) : 0u;
  for (uint32_t i = start; i < total; i++) {
    Serial.println(ring[i % cap]);
  }
  Serial.printf("--- END %s ---\n", path);
  return true;
}

static bool print_consumed_records_tail(const char *path, uint32_t max_records) {
  if (!path) return false;
  if (!ensure_fs_mounted()) return false;

  File f = SPIFFS.open(path, "r");
  if (!f) {
    Serial.printf("%s open FAIL (missing?)\n", path);
    return false;
  }

  const size_t sz = (size_t)f.size();
  if ((sz % 4u) != 0u) {
    Serial.printf("--- %s ---\n", path);
    Serial.println("ERROR: corrupt consumed record (size not multiple of 4)");
    Serial.printf("--- END %s ---\n", path);
    f.close();
    return false;
  }

  const uint32_t total = (uint32_t)(sz / 4u);
  const uint32_t cap = (max_records > 50u) ? 50u : max_records;
  const uint32_t to_print = (total > cap) ? cap : total;
  const uint32_t start_idx = total - to_print;

  Serial.printf("--- %s ---\n", path);
  if (total > cap) {
    const uint32_t omitted = total - cap;
    Serial.printf("Log file too long. omitting preceding %lu records.\n", (unsigned long)omitted);
  }

  if (!f.seek(start_idx * 4u, SeekSet)) {
    Serial.println("ERROR: seek failed");
    Serial.printf("--- END %s ---\n", path);
    f.close();
    return false;
  }

  uint8_t b[4];
  for (uint32_t i = 0; i < to_print; i++) {
    const int r = f.read(b, sizeof(b));
    if (r != (int)sizeof(b)) {
      Serial.println("ERROR: short read");
      break;
    }
    const uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    const uint32_t idx = start_idx + i;
    if (v == 0x00000000u) {
      Serial.printf("[%lu] 0 (USERSET marker; next entry is next-serial seed)\n", (unsigned long)idx);
    } else {
      Serial.printf("[%lu] %lu\n", (unsigned long)idx, (unsigned long)v);
    }
  }

  Serial.printf("--- END %s ---\n", path);
  f.close();
  return true;
}

static bool cmd_print_logs() {
  bool ok = true;
  ok = print_text_file_tail_lines(serial_log::log_path(), /*max_lines=*/50) && ok;
  ok = print_consumed_records_tail(serial_log::consumed_records_path(), /*max_records=*/50) && ok;
  return ok;
}

static void cmd_print_wifi_ap_status() {
  const wifi_mode_t mode = WiFi.getMode();
  const bool ap_enabled = (mode == WIFI_MODE_AP) || (mode == WIFI_MODE_APSTA);

  Serial.printf("WiFi mode: %d (%s)\n", (int)mode,
                (mode == WIFI_MODE_NULL)   ? "OFF"
                : (mode == WIFI_MODE_STA)  ? "STA"
                : (mode == WIFI_MODE_AP)   ? "AP"
                : (mode == WIFI_MODE_APSTA) ? "AP+STA"
                                             : "UNKNOWN");
  Serial.printf("WiFi AP enabled: %s\n", ap_enabled ? "YES" : "NO");

  if (ap_enabled) {
    const IPAddress ip = WiFi.softAPIP();
    Serial.printf("WiFi AP IP: %s\n", ip.toString().c_str());
    Serial.printf("WiFi AP stations: %d\n", WiFi.softAPgetStationNum());
  }
}

void loop() {
  // --- Production button handling (runs even if no Serial activity) ---
  static bool button_raw_last = true;     // pull-up => HIGH when released
  static bool button_stable = true;
  static uint32_t button_last_change_ms = 0;
  static bool button_armed = true;        // re-arm once released

  const bool raw = (digitalRead(k_prod_button_pin) != LOW);
  const uint32_t now_ms = millis();

  if (raw != button_raw_last) {
    button_raw_last = raw;
    button_last_change_ms = now_ms;
  }

  if ((uint32_t)(now_ms - button_last_change_ms) >= k_button_debounce_ms) {
    if (button_stable != raw) {
      button_stable = raw;

      // Falling edge (released->pressed): stable goes HIGH->LOW.
      if (!button_stable && button_armed) {
        button_armed = false;
        run_production_sequence("GPIO45 button");
      }

      // Re-arm after release.
      if (button_stable) {
        button_armed = true;
      }
    }
  }

  // --- Serial command handling ---
  if (!Serial.available()) {
    delay(10);
    return;
  }

  const char c = (char)Serial.read();

  // Ignore whitespace/newlines from terminal.
  // NOTE: Space is a real command (<space>) used for production programming.
  if (c == '\n' || c == '\r') {
    return;
  }

  print_user_pressed_banner(c);

  switch (c) {
    case 'f':
      if (ensure_fs_mounted()) firmware_fs::print_status();
      break;

    case 'F':
      {
        String fw_path;
        const bool ok = select_firmware_path(fw_path);
        Serial.println(ok ? "Firmware file selection OK" : "Firmware file selection FAIL");
      }
      break;

    case ' ':
      run_production_sequence("Serial <space>");
      break;

    case 'h':
    case '?':
      print_help();
      break;

    case 'i':
      print_idcode_attempt();
      break;

    case 's':
      cmd_consume_serial_record_only();
      break;

    case 'S':
      {
        // Parse decimal digits that follow 'S' on the same line, e.g. "S12345\n".
        // If no digits are present, do nothing (keeps it safe for accidental presses).
        String digits;
        const uint32_t start_ms = millis();
        while ((uint32_t)(millis() - start_ms) < 250) {
          if (!Serial.available()) {
            delay(1);
            continue;
          }
          const char d = (char)Serial.peek();
          if (d == '\n' || d == '\r') {
            (void)Serial.read();
            break;
          }
          if (d >= '0' && d <= '9') {
            digits += (char)Serial.read();
          } else {
            // Consume unexpected char and stop.
            (void)Serial.read();
            break;
          }
        }

        if (digits.length() == 0) {
          Serial.println("Set serial: no digits provided; use S<serial> (example: S1000)");
          break;
        }

        const uint32_t next = (uint32_t)digits.toInt();
        if (!ensure_fs_mounted()) {
          Serial.println("Set serial FAIL (FS not mounted)");
          break;
        }
        if (!serial_log::user_set_serial_next(next)) {
          Serial.println("Set serial FAIL (persist)");
          break;
        }
        Serial.printf("Set serial OK: USERSET_%lu\n", (unsigned long)next);
        if (serial_log::has_serial_next()) {
          Serial.printf("Next serial (loaded): %lu\n", (unsigned long)serial_log::serial_next());
        } else {
          Serial.println("Next serial (loaded): NOT SET (use WiFi UI to set it)");
        }
      }
      break;

    case 'l':
      cmd_print_logs();
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
      cmd_print_wifi_ap_status();
      break;

    default:
      Serial.printf("Unknown command '%c' (0x%02X). Press 'h' for help.\n", c, (unsigned)c);
      break;
  }
}
