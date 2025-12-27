#include "servomotor_upgrade.h"

#include <SPIFFS.h>

#include <vector>

// Servomotor Arduino library (vendored into lib/Servomotor by tools/stage_servomotor_assets.py)
#include <Servomotor.h>

#include <Commands.h>

// CRC32 helper lives in the Servomotor Arduino library.
#include <Communication.h>

#include "tee_log.h"

// Route all Serial prints in this file into the RAM terminal buffer as well.
#define Serial tee_log::out()

namespace servomotor_upgrade {

// Match the Arduino library example pin assignment.
// ESP32-S3 DevKitC UART1 pins:
//   TX=GPIO4, RX=GPIO5
// Note: RS485 direction control is assumed to be handled by the transceiver (auto-direction).
static constexpr int8_t k_rs485_txd = 4;
static constexpr int8_t k_rs485_rxd = 5;

// Constants mirror the known-good host tool:
// [`python_programs/upgrade_firmware.py`](../../Servomotor/python_programs/upgrade_firmware.py:59)
static constexpr uint32_t k_flash_page_size = 2048;
static constexpr uint8_t k_model_code_len = 8;
static constexpr uint8_t k_first_firmware_page_number = 5;
static constexpr uint8_t k_last_firmware_page_number = 30;

static constexpr uint32_t k_wait_for_reset_ms = 70;

static bool read_all(File &f, uint8_t *dst, size_t n) {
  if (!dst && n) return false;
  size_t got = 0;
  while (got < n) {
    const int r = f.read(dst + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

// NOTE: The SYSTEM_RESET command causes the target to reset immediately.
// In practice (and in your Python upgrader), we do not require a response packet.
// The auto-generated Arduino library method [`arduino.Servomotor::systemReset()`](../../Servomotor/Arduino_library/Servomotor.cpp:1084)
// calls getResponse() and will therefore timeout if the device resets before replying.
//
// To avoid a false failure, we send the reset packet directly and do not wait for a response.
static void send_system_reset_no_wait(Communication &comm, uint64_t unique_id) {
  comm.sendCommandByUniqueId(unique_id, SYSTEM_RESET, /*payload=*/nullptr, /*payloadSize=*/0);
  comm.flush();
}

static void send_firmware_page_no_wait(Communication &comm, uint64_t unique_id, const uint8_t page2058[2058]) {
  comm.sendCommandByUniqueId(unique_id, FIRMWARE_UPGRADE, page2058, /*payloadSize=*/2058);
  comm.flush();
}

bool upgrade_main_firmware_by_unique_id(const char *firmware_path) {
  
}

}  // namespace servomotor_upgrade

#undef Serial

  if (!firmware_path || firmware_path[0] == 0) {
    Serial.println("ERROR: servomotor firmware path missing");
    return false;
  }
  if (unique_id == 0) {
    Serial.println("ERROR: unique_id is 0 (invalid)");
    return false;
  }
  if (!SPIFFS.begin(false, "/spiffs", 10, "fwfs")) {
    Serial.println("ERROR: SPIFFS not mounted");
    return false;
  }

  File f = SPIFFS.open(firmware_path, "r");
  if (!f) {
    Serial.printf("ERROR: could not open servomotor firmware file: %s\n", firmware_path);
    return false;
  }
  const size_t file_size = (size_t)f.size();
  if (file_size < (size_t)k_model_code_len + 1u) {
    Serial.println("ERROR: servomotor firmware file too small");
    return false;
  }

  uint8_t model_code[k_model_code_len];
  uint8_t fw_compat = 0;
  if (!read_all(f, model_code, sizeof(model_code))) return false;
  if (!read_all(f, &fw_compat, 1)) return false;

  const size_t firmware_data_size = file_size - (size_t)k_model_code_len - 1u;
  // Mirror python MINIMUM_FIRWARE_SIZE = FLASH_PAGE_SIZE - CRC32_SIZE.
  if (firmware_data_size < (k_flash_page_size - 4u)) {
    Serial.printf("ERROR: firmware payload too small (%lu bytes)\n", (unsigned long)firmware_data_size);
    return false;
  }

  // Read firmware payload.
  // NOTE: The upgrade algorithm rewrites the first 32-bit word and appends a CRC32,
  // so we allocate +8 bytes headroom.
  const size_t max_payload = (size_t)(k_last_firmware_page_number - k_first_firmware_page_number + 1u) *
                             (size_t)k_flash_page_size;
  if (firmware_data_size > max_payload) {
    Serial.printf("ERROR: firmware payload too large for allowed page range (%lu > %lu bytes)\n",
                  (unsigned long)firmware_data_size, (unsigned long)max_payload);
    return false;
  }

  // Read the remainder into RAM.
  // (The expected size for this STM32G0 target is small enough to fit.)
  std::vector<uint8_t> data;
  data.resize(firmware_data_size);
  if (!read_all(f, data.data(), data.size())) {
    Serial.println("ERROR: failed to read firmware payload");
    return false;
  }
  f.close();

  // Pad to multiple of 4 with 0x00 (matches python).
  while ((data.size() & 0x3u) != 0u) data.push_back(0x00);
  if (data.size() < 8u) {
    Serial.println("ERROR: firmware payload unexpectedly small after padding");
    return false;
  }

  const uint32_t firmware_size_words = (uint32_t)((data.size() >> 2) - 1u);
  const uint32_t firmware_crc = calculate_crc32(data.data() + 4u, data.size() - 4u);

  // Build transformed payload:
  //   [size_words:4] + data[4:] + [crc32:4]
  std::vector<uint8_t> tx;
  tx.resize(4u + (data.size() - 4u) + 4u);
  memcpy(tx.data(), &firmware_size_words, 4u);
  memcpy(tx.data() + 4u, data.data() + 4u, data.size() - 4u);
  memcpy(tx.data() + 4u + (data.size() - 4u), &firmware_crc, 4u);

  Serial.printf("Servomotor upgrade: model='%.8s' compat=%u payload=%lu bytes (tx=%lu bytes)\n",
                (const char *)model_code, (unsigned)fw_compat, (unsigned long)firmware_data_size,
                (unsigned long)tx.size());
  Serial.printf("Servomotor upgrade: size_words=%lu crc32=0x%08lX unique_id=0x%08lX%08lX\n",
                (unsigned long)firmware_size_words, (unsigned long)firmware_crc,
                (unsigned long)(unique_id >> 32), (unsigned long)(unique_id & 0xFFFFFFFFu));

  // Prepare comms.
  // IMPORTANT:
  // - We use the Servomotor Arduino library for packet formatting.
  // - The library's defaults are Serial1 @ 230400.
  // - Pins must be set explicitly (match Arduino example).
  // - We do not manage RS485 direction-control (auto-direction transceiver).
  static HardwareSerial &k_rs485_serial = Serial1;

  // NOTE: Do not specify baud explicitly (per project guidance). The library default is used.
  Servomotor m('X', k_rs485_serial, /*rxPin=*/k_rs485_rxd, /*txPin=*/k_rs485_txd);
  m.useUniqueId(unique_id);

  // Pre-flight: try a cheap query to confirm we can hear the device.
  // If this times out, the upgrade is unlikely to work.
  Serial.println("Servomotor upgrade: preflight GET_FIRMWARE_VERSION...");
  (void)m.getFirmwareVersion(unique_id);
  if (m.getError() != 0) {
    Serial.printf("WARN: GET_FIRMWARE_VERSION failed errno=%d (RS485 link may be down or unique_id wrong)\n",
                  (int)m.getError());
  }

  Serial.println("Servomotor upgrade: SYSTEM_RESET -> enter bootloader");
  send_system_reset_no_wait(comm, unique_id);
  delay(k_wait_for_reset_ms);

  uint8_t page[2058];
  memcpy(page, model_code, k_model_code_len);
  page[k_model_code_len] = fw_compat;

  uint8_t page_number = k_first_firmware_page_number;
  size_t off = 0;
  while (off < tx.size()) {
    if (page_number > k_last_firmware_page_number) {
      Serial.println("ERROR: firmware too large for allowed page range");
      return false;
    }

    page[k_model_code_len + 1u] = page_number;

    const size_t remain = tx.size() - off;
    const size_t take = (remain >= k_flash_page_size) ? k_flash_page_size : remain;
    memcpy(page + k_model_code_len + 2u, tx.data() + off, take);
    if (take < k_flash_page_size) {
      memset(page + k_model_code_len + 2u + take, 0x00, k_flash_page_size - take);
    }

    Serial.printf("Servomotor upgrade: writing page %u (offset %lu)\n", (unsigned)page_number, (unsigned long)off);

    // Preferred path: send page and wait for a response (targeted unique_id mode).
    // If the device resets or does not ACK, the auto-generated library will timeout.
    m.firmwareUpgrade(unique_id, page);
    if (m.getError() != 0) {
      Serial.printf("WARN: firmwareUpgrade timed out at page %u errno=%d; falling back to no-ACK mode\n",
                    (unsigned)page_number, (int)m.getError());

      // Fallback: broadcast-like behavior (no response expected). This mirrors the
      // python upgrader's rate limiting when using alias 255.
      static constexpr uint32_t k_delay_after_each_page_ms = 180;

      // Send current page without waiting.
      send_firmware_page_no_wait(comm, unique_id, page);
      delay(k_delay_after_each_page_ms);

      // Send remaining pages without waiting.
      off += take;
      page_number++;
      while (off < tx.size()) {
        if (page_number > k_last_firmware_page_number) {
          Serial.println("ERROR: firmware too large for allowed page range");
          return false;
        }

        page[k_model_code_len + 1u] = page_number;
        const size_t remain2 = tx.size() - off;
        const size_t take2 = (remain2 >= k_flash_page_size) ? k_flash_page_size : remain2;
        memcpy(page + k_model_code_len + 2u, tx.data() + off, take2);
        if (take2 < k_flash_page_size) {
          memset(page + k_model_code_len + 2u + take2, 0x00, k_flash_page_size - take2);
        }

        Serial.printf("Servomotor upgrade(no-ACK): writing page %u (offset %lu)\n", (unsigned)page_number,
                      (unsigned long)off);
        send_firmware_page_no_wait(comm, unique_id, page);
        delay(k_delay_after_each_page_ms);

        off += take2;
        page_number++;
      }

      // Post-upgrade reset (no wait).
      Serial.println("Servomotor upgrade: SYSTEM_RESET -> boot into new firmware");
      send_system_reset_no_wait(comm, unique_id);
      Serial.println("Servomotor upgrade OK (no-ACK mode; please verify device firmware version manually)");
      return true;
    }

    off += take;
    page_number++;
  }

  Serial.println("Servomotor upgrade: SYSTEM_RESET -> boot into new firmware");
  send_system_reset_no_wait(comm, unique_id);

  Serial.println("Servomotor upgrade OK");
  return true;
}

bool print_product_info_by_unique_id(Servomotor m, uint64_t unique_id) {
  if (unique_id == 0) {
    Serial.println("ERROR: unique_id is 0 (invalid)");
    return false;
  }

  // Target by unique_id.
  const getProductInfoResponse r = m.getProductInfo();
  const int err = m.getError();
  if (err != 0) {
    Serial.printf("ERROR: getProductInfo failed errno=%d\n", err);
    return false;
  }

  // productCode is not guaranteed to be NUL-terminated.
  char product_code[sizeof(r.productCode) + 1];
  memcpy(product_code, r.productCode, sizeof(r.productCode));
  product_code[sizeof(r.productCode)] = 0;

  // hardwareVersion encoding is device-defined; print both hex and byte-split for convenience.
  const uint32_t hw = r.hardwareVersion;

  Serial.println("Servomotor GET_PRODUCT_INFO response:");
  Serial.printf("  productCode: '%s'\n", product_code);
  Serial.printf("  firmwareCompatibility: %u\n", (unsigned)r.firmwareCompatibility);
  Serial.printf("  hardwareVersion: 0x%08lX (bytes: %u.%u.%u.%u)\n", (unsigned long)hw,
                (unsigned)((hw >> 24) & 0xFFu), (unsigned)((hw >> 16) & 0xFFu), (unsigned)((hw >> 8) & 0xFFu),
                (unsigned)(hw & 0xFFu));
  Serial.printf("  serialNumber: %lu\n", (unsigned long)r.serialNumber);
  Serial.printf("  uniqueId: 0x%08lX%08lX\n", (unsigned long)(r.uniqueId >> 32), (unsigned long)(r.uniqueId & 0xFFFFFFFFu));
  Serial.printf("  reserved: 0x%08lX\n", (unsigned long)r.reserved);

  return true;
}

}  // namespace servomotor_upgrade

#undef Serial
