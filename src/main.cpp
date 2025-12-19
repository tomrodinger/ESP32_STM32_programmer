#include <Arduino.h>

#include "binary.h"
#include "stm32g0_prog.h"
#include "swd_min.h"

static const swd_min::Pins PINS(35, 36, 37);

static void print_help() {
  Serial.println("Commands:");
  Serial.println("  h = help");
  Serial.println("  i = reset + read DP IDCODE");
  Serial.println("  e = erase entire flash (mass erase)");
  Serial.println("  w = write firmware to flash");
  Serial.println("  v = verify firmware in flash (dumps bytes read + mismatch count)");
  Serial.println("  a = all: connect+halt, erase, write, verify");
}

static bool print_idcode_attempt() {
  uint8_t ack = 0;
  uint32_t idcode = 0;

  swd_min::reset_and_switch_to_swd();

  const bool ok = swd_min::read_idcode(&idcode, &ack);

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
  if (!cmd_connect()) return false;
  const bool ok = stm32g0_prog::flash_mass_erase();
  Serial.println(ok ? "Erase OK" : "Erase FAIL");
  return ok;
}

static bool cmd_write() {
  if (!cmd_connect()) return false;
  const bool ok = stm32g0_prog::flash_program(stm32g0_prog::FLASH_BASE, firmware_bin, firmware_bin_len);
  Serial.println(ok ? "Write OK" : "Write FAIL");
  return ok;
}

static bool cmd_verify() {
  if (!cmd_connect()) return false;
  const bool ok = stm32g0_prog::flash_verify_and_dump(stm32g0_prog::FLASH_BASE, firmware_bin, firmware_bin_len);
  Serial.println(ok ? "Verify OK (all bytes match)" : "Verify FAIL (see mismatch count above)");
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

  switch (c) {
    case 'h':
    case '?':
      print_help();
      break;

    case 'i':
      print_idcode_attempt();
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
      // Ignore whitespace/newlines from terminal.
      if (c != '\n' && c != '\r' && c != ' ') {
        Serial.printf("Unknown command '%c' (0x%02X). Press 'h' for help.\n", c, (unsigned)c);
      }
      break;
  }
}
