#include "mode2_loop.h"

#include <Arduino.h>

#include <cmath>

// Servomotor Arduino library (vendored into lib/Servomotor)
#include <Servomotor.h>

// For COMMUNICATION_ERROR_TIMEOUT
#include <Communication.h>

#include "tee_log.h"
#include "unit_context.h"

#include "servomotor_upgrade.h"

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
  LOG().println("  R = system reset (motor -> bootloader)");
  LOG().println("  D = detect devices (broadcast; prints all responses)");
  LOG().println("  e = enable MOSFETs");
  LOG().println("  d = disable MOSFETs");
  LOG().println("  t = trapezoid move (1 rotation for 1 second)");
  LOG().println("  p = get comprehensive position (prints read-back values)");
  LOG().println("  P = get comprehensive position (reference device via alias 'X')");
  LOG().println("  s = get status (expects fatalErrorCode == 0)");
  LOG().println("  v = get supply voltage (expects within 5% of 20V)");
  LOG().println("  c = get temperature (expects within 20% of 30C)");
  LOG().println("  i = get product info (RS485)");
  LOG().println("  u = upgrade firmware over RS485 (unique ID addressing)");
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

static void print_motor_call_error(const char *op, const Servomotor &motor) {
  const int err = motor.getError();
  if (err == 0) {
    return;
  }
  if (err == COMMUNICATION_ERROR_TIMEOUT) {
    LOG().printf("ERROR: %s timed out\n", op);
  } else {
    LOG().printf("ERROR: %s failed errno=%d\n", op, err);
  }
}

static void cmd_system_reset(Servomotor &motor) {
  motor.systemReset();
  print_motor_call_error("systemReset", motor);
}

static void cmd_enable_mosfets(Servomotor &motor) {
  motor.enableMosfets();
  print_motor_call_error("enableMosfets", motor);
}

static void cmd_disable_mosfets(Servomotor &motor) {
  motor.disableMosfets();
  print_motor_call_error("disableMosfets", motor);
}

static void cmd_trapezoid_move_1_rotation_1_second(Servomotor &motor) {
  // Default unit settings for the vendored library are:
  // - position: SHAFT_ROTATIONS
  // - time: SECONDS
  // so these are expressed as 1 rotation over 1 second.
  motor.trapezoidMove(1.0f, 1.0f);
  print_motor_call_error("trapezoidMove", motor);
}

static void cmd_get_comprehensive_position(Servomotor &motor) {
  const getComprehensivePositionResponse r = motor.getComprehensivePositionRaw();
  const int err = motor.getError();
  if (err != 0) {
    if (err == COMMUNICATION_ERROR_TIMEOUT) {
      LOG().println("ERROR: getComprehensivePosition timed out");
    } else {
      LOG().printf("ERROR: getComprehensivePosition failed errno=%d\n", err);
    }
    return;
  }

  LOG().println("Servomotor GET_COMPREHENSIVE_POSITION response (raw):");
  LOG().printf("  commandedPosition: %lld\n", (long long)r.commandedPosition);
  LOG().printf("  hallSensorPosition: %lld\n", (long long)r.hallSensorPosition);
  LOG().printf("  externalEncoderPosition: %ld\n", (long)r.externalEncoderPosition);
}

static void cmd_get_comprehensive_position_reference(Servomotor &ref_motor) {
  // Reference device is addressed by alias (standard addressing), to avoid
  // dependence on unit_context / unique_id.
  LOG().println("Reference device (alias 'X') GET_COMPREHENSIVE_POSITION:");

  const getComprehensivePositionResponse r = ref_motor.getComprehensivePositionRaw();
  const int err = ref_motor.getError();
  if (err != 0) {
    if (err == COMMUNICATION_ERROR_TIMEOUT) {
      LOG().println("ERROR: getComprehensivePosition(ref) timed out");
    } else {
      LOG().printf("ERROR: getComprehensivePosition(ref) failed errno=%d\n", err);
    }
    return;
  }

  LOG().println("Servomotor GET_COMPREHENSIVE_POSITION response (raw) [ref]:");
  LOG().printf("  commandedPosition: %lld\n", (long long)r.commandedPosition);
  LOG().printf("  hallSensorPosition: %lld\n", (long long)r.hallSensorPosition);
  LOG().printf("  externalEncoderPosition: %ld\n", (long)r.externalEncoderPosition);
}

static bool ensure_dut_unique_id_configured(Servomotor &motor) {
  if (motor.isUsingExtendedAddressing()) {
    return true;
  }

  const unit_context::Context ctx = unit_context::get();
  if (ctx.valid && ctx.unique_id != 0) {
    motor.useUniqueId(ctx.unique_id);
    return true;
  }

  LOG().println("ERROR: DUT unique_id not known (program a unit first in Mode 1)");
  return false;
}

static bool cmd_detect_devices_and_print(Servomotor &broadcast_motor) {
  LOG().println("Servomotor DETECT_DEVICES (broadcast) ...");

  int count = 0;

  // First response is returned by detectDevices(); subsequent ones are retrieved
  // by calling detectDevicesGetAnotherResponse() until timeout.
  const detectDevicesResponse r0 = broadcast_motor.detectDevices();
  int err = broadcast_motor.getError();
  if (err != 0) {
    if (err == COMMUNICATION_ERROR_TIMEOUT) {
      LOG().println("No devices responded (timeout)");
    } else {
      LOG().printf("ERROR: detectDevices failed errno=%d\n", err);
    }
    return false;
  }

  {
    ++count;
    LOG().printf("  [%d] alias='%c' (0x%02X) uniqueId=0x%08lX%08lX\n", count,
                 (r0.alias >= 32 && r0.alias <= 126) ? (char)r0.alias : '?', (unsigned)r0.alias,
                 (unsigned long)(r0.uniqueId >> 32), (unsigned long)(r0.uniqueId & 0xFFFFFFFFu));
  }

  while (true) {
    const detectDevicesResponse r = broadcast_motor.detectDevicesGetAnotherResponse();
    err = broadcast_motor.getError();
    if (err != 0) {
      if (err == COMMUNICATION_ERROR_TIMEOUT) {
        break;  // Done.
      }
      LOG().printf("ERROR: detectDevicesGetAnotherResponse failed errno=%d\n", err);
      break;
    }

    ++count;
    LOG().printf("  [%d] alias='%c' (0x%02X) uniqueId=0x%08lX%08lX\n", count,
                 (r.alias >= 32 && r.alias <= 126) ? (char)r.alias : '?', (unsigned)r.alias,
                 (unsigned long)(r.uniqueId >> 32), (unsigned long)(r.uniqueId & 0xFFFFFFFFu));
  }

  LOG().printf("Detect devices: %d response(s)\n", count);
  return count > 0;
}

static bool within_rel_tol(float measured, float expected, float rel_tol) {
  const float tol = std::fabs(expected) * rel_tol;
  return std::fabs(measured - expected) <= tol;
}

static bool cmd_get_status_and_check(Servomotor &motor) {
  const getStatusResponse r = motor.getStatus();
  const int err = motor.getError();
  if (err != 0) {
    if (err == COMMUNICATION_ERROR_TIMEOUT) {
      LOG().println("ERROR: getStatus timed out");
    } else {
      LOG().printf("ERROR: getStatus failed errno=%d\n", err);
    }
    LOG().println("Mode2 getStatus FAIL");
    return false;
  }

  LOG().println("Servomotor GET_STATUS response:");
  LOG().printf("  statusFlags: 0x%04X\n", (unsigned)r.statusFlags);
  LOG().printf("  fatalErrorCode: %u\n", (unsigned)r.fatalErrorCode);

  const bool ok = (r.fatalErrorCode == 0);
  LOG().println(ok ? "Mode2 getStatus OK" : "Mode2 getStatus FAIL");
  return ok;
}

static bool cmd_get_supply_voltage_and_check(Servomotor &motor) {
  const float v = motor.getSupplyVoltage();
  const int err = motor.getError();
  if (err != 0) {
    if (err == COMMUNICATION_ERROR_TIMEOUT) {
      LOG().println("ERROR: getSupplyVoltage timed out");
    } else {
      LOG().printf("ERROR: getSupplyVoltage failed errno=%d\n", err);
    }
    LOG().println("Mode2 getSupplyVoltage FAIL");
    return false;
  }

  static constexpr float k_expected_v = 20.0f;
  static constexpr float k_rel_tol = 0.05f;
  static constexpr float k_min = k_expected_v * (1.0f - k_rel_tol);
  static constexpr float k_max = k_expected_v * (1.0f + k_rel_tol);
  const bool ok = within_rel_tol(v, k_expected_v, k_rel_tol);

  LOG().println("Servomotor GET_SUPPLY_VOLTAGE response:");
  LOG().printf("  supplyVoltage: %.3f V\n", (double)v);
  LOG().printf("  expected: %.3f V +/- %.1f%% (range [%.3f, %.3f])\n", (double)k_expected_v, (double)(k_rel_tol * 100.0f),
              (double)k_min, (double)k_max);
  LOG().println(ok ? "Mode2 getSupplyVoltage OK" : "Mode2 getSupplyVoltage FAIL");
  return ok;
}

static bool cmd_get_temperature_and_check(Servomotor &motor) {
  const float t = motor.getTemperature();
  const int err = motor.getError();
  if (err != 0) {
    if (err == COMMUNICATION_ERROR_TIMEOUT) {
      LOG().println("ERROR: getTemperature timed out");
    } else {
      LOG().printf("ERROR: getTemperature failed errno=%d\n", err);
    }
    LOG().println("Mode2 getTemperature FAIL");
    return false;
  }

  static constexpr float k_expected_t = 30.0f;
  static constexpr float k_rel_tol = 0.20f;
  static constexpr float k_min = k_expected_t * (1.0f - k_rel_tol);
  static constexpr float k_max = k_expected_t * (1.0f + k_rel_tol);
  const bool ok = within_rel_tol(t, k_expected_t, k_rel_tol);

  LOG().println("Servomotor GET_TEMPERATURE response:");
  LOG().printf("  temperature: %.3f C\n", (double)t);
  LOG().printf("  expected: %.3f C +/- %.1f%% (range [%.3f, %.3f])\n", (double)k_expected_t, (double)(k_rel_tol * 100.0f),
              (double)k_min, (double)k_max);
  LOG().println(ok ? "Mode2 getTemperature OK" : "Mode2 getTemperature FAIL");
  return ok;
}

namespace mode2_loop {

void run() {
  print_mode2_banner();

  // Mirror the minimal Arduino library example style.
  // Example RS485 pins for ESP32 (matches the library example and our hardware).
  // NOTE: For the DUT, we typically switch to unique-ID addressing (unit_context)
  // so the alias is only a fallback.
  static constexpr uint8_t k_dut_alias = 0;
  static constexpr uint8_t k_ref_alias = 'X';
  static constexpr uint8_t k_all_alias = ALL_ALIAS;

  static constexpr int8_t k_rs485_txd = 4;
  static constexpr int8_t k_rs485_rxd = 5;
  Servomotor motor(k_dut_alias, Serial1, k_rs485_rxd, k_rs485_txd);
  Servomotor ref_motor(k_ref_alias, Serial1, k_rs485_rxd, k_rs485_txd);
  Servomotor all_motor(k_all_alias, Serial1, k_rs485_rxd, k_rs485_txd);

  // NOTE: We intentionally do *not* fall back to a DUT alias when unique_id is unknown.
  // We lazily configure unique_id when a DUT command is invoked.

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

      case 'R':
        if (ensure_dut_unique_id_configured(motor)) {
          cmd_system_reset(motor);
        }
        break;

      case 'D':
        (void)cmd_detect_devices_and_print(all_motor);
        break;

      case 'e':
        if (ensure_dut_unique_id_configured(motor)) {
          cmd_enable_mosfets(motor);
        }
        break;

      case 'd':
        if (ensure_dut_unique_id_configured(motor)) {
          cmd_disable_mosfets(motor);
        }
        break;

      case 't':
        if (ensure_dut_unique_id_configured(motor)) {
          cmd_trapezoid_move_1_rotation_1_second(motor);
        }
        break;

      case 'p':
        if (ensure_dut_unique_id_configured(motor)) {
          cmd_get_comprehensive_position(motor);
        }
        break;

      case 'P':
        cmd_get_comprehensive_position_reference(ref_motor);
        break;

      case 's':
        if (ensure_dut_unique_id_configured(motor)) {
          (void)cmd_get_status_and_check(motor);
        }
        break;

      case 'v':
        if (ensure_dut_unique_id_configured(motor)) {
          (void)cmd_get_supply_voltage_and_check(motor);
        }
        break;

      case 'c':
        if (ensure_dut_unique_id_configured(motor)) {
          (void)cmd_get_temperature_and_check(motor);
        }
        break;

      case 'i':
        if (ensure_dut_unique_id_configured(motor)) {
          cmd_print_product_info(motor);
        }
        break;

      case 'u': {
        const unit_context::Context ctx2 = unit_context::get();
        if (!ctx2.valid || ctx2.unique_id == 0) {
          LOG().println("ERROR: no valid unique_id in unit_context (program a unit first in Mode 1)");
          break;
        }

        const bool ok = servomotor_upgrade::upgrade_main_firmware_by_unique_id(motor, ctx2.unique_id);
        if (!ok) {
          LOG().println("ERROR: firmware upgrade failed (see log above)");
        }
      } break;

      default:
        LOG().printf("Unknown command '%c'. Press 'h' for help.\n", c);
        break;
    }
  }
}

}  // namespace mode2_loop
