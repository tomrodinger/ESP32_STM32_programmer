#include "mode2_loop.h"

#include <Arduino.h>

// Servomotor Arduino library (vendored into lib/Servomotor)
#include <Servomotor.h>

// For COMMUNICATION_ERROR_TIMEOUT
#include <Communication.h>

#include "tee_log.h"
#include "unit_context.h"

static inline Print &LOG() { return tee_log::out(); }

static void print_user_pressed_banner(char c) {
  if (c >= 32 && c <= 126) {
    LOG().printf("=== [Mode 2] User pressed '%c' (0x%02X) ===\n", c, (unsigned char)c);
  } else {
    LOG().printf("=== [Mode 2] User pressed 0x%02X ===\n", (unsigned char)c);
  }
}

static void print_help() {
  LOG().println("Mode 2: RS485 Testing Mode");
  LOG().println("Commands:");
  LOG().println("  h = help");
  LOG().println("  1 = switch to Mode 1 (SWD Programming)");
  LOG().println("  2 = stay in Mode 2 (this mode)");
  LOG().println("  t = test (prints 'Testing... test done!')");
  LOG().println("  p = get product info (RS485)");
}

static void print_mode2_banner() {
  LOG().println("========================================");
  LOG().println("          MODE 2: RS485 Testing        ");
  LOG().println("========================================");
  LOG().println("Press 'h' for help, '1' to switch to SWD mode");
}

static void cmd_print_product_info(Servomotor &motor) {
  const getProductInfoResponse r = motor.getProductInfo();
  const int err = motor.getError();
  if (err != 0) {
    if (err == COMMUNICATION_ERROR_TIMEOUT) {
      LOG().println("ERROR: getProductInfo timed out");
    } else {
      LOG().printf("ERROR: getProductInfo failed errno=%d\n", err);
    }
    return;
  }

  // productCode is not guaranteed to be NUL-terminated.
  char product_code[sizeof(r.productCode) + 1];
  memcpy(product_code, r.productCode, sizeof(r.productCode));
  product_code[sizeof(r.productCode)] = 0;

  LOG().println("Servomotor GET_PRODUCT_INFO response:");
  LOG().printf("  productCode: '%s'\n", product_code);
  LOG().printf("  firmwareCompatibility: %u\n", (unsigned)r.firmwareCompatibility);
  LOG().printf("  hardwareVersion: %u.%u.%u\n", (unsigned)r.hardwareVersion.major, (unsigned)r.hardwareVersion.minor,
              (unsigned)r.hardwareVersion.patch);
  LOG().printf("  serialNumber: %lu\n", (unsigned long)r.serialNumber);
  LOG().printf("  uniqueId: 0x%08lX%08lX\n", (unsigned long)(r.uniqueId >> 32),
               (unsigned long)(r.uniqueId & 0xFFFFFFFFu));
  LOG().printf("  reserved: 0x%08lX\n", (unsigned long)r.reserved);
}

namespace mode2_loop {

void run() {
  print_mode2_banner();

  // Mirror the minimal Arduino library example style.
  // Example RS485 pins for ESP32 (matches the library example and our hardware).
  static constexpr uint8_t k_alias = 'X';

  static constexpr int8_t k_rs485_txd = 4;
  static constexpr int8_t k_rs485_rxd = 5;
  Servomotor motor(k_alias, Serial1, k_rs485_rxd, k_rs485_txd);

  // Central place: set unique_id once and keep it inside the motor object.
  const unit_context::Context ctx = unit_context::get();
  if (ctx.valid && ctx.unique_id != 0) {
    motor.useUniqueId(ctx.unique_id);
  } else {
    LOG().println("WARN: no valid unit_context unique_id (program a unit first in Mode 1)");
  }

  while (true) {
    if (!Serial.available()) {
      delay(10);
      continue;
    }

    const char c = (char)Serial.read();

    // Ignore whitespace/newlines.
    if (c == '\n' || c == '\r' || c == ' ') {
      continue;
    }

    print_user_pressed_banner(c);

    switch (c) {
      case 'h':
      case '?':
        print_help();
        break;

      case '1':
        LOG().println("Switching to Mode 1 (SWD Programming)...");
        return;

      case '2':
        LOG().println("Already in Mode 2 (RS485 Testing)");
        break;

      case 't':
        LOG().println("Testing... test done!");
        break;

      case 'p':
        cmd_print_product_info(motor);
        break;

      default:
        LOG().printf("Unknown command '%c'. Press 'h' for help.\n", c);
        break;
    }
  }
}

}  // namespace mode2_loop
