#include "servomotor_upgrade.h"

#include <SPIFFS.h>

#include <vector>

// Servomotor Arduino library (vendored into lib/Servomotor)
#include <Servomotor.h>

// For COMMUNICATION_ERROR_TIMEOUT and calculate_crc32().
#include <Communication.h>

#include "firmware_fs.h"
#include "program_state.h"
#include "tee_log.h"

// Route all prints in this file into the RAM terminal buffer as well.
#define Serial tee_log::out()

namespace servomotor_upgrade {

static constexpr size_t k_flash_page_size = 2048;
static constexpr size_t k_model_code_len = 8;
static constexpr uint8_t k_first_firmware_page_number = 5;
static constexpr uint8_t k_last_firmware_page_number = 30;

// Match the known-good host upgrader timing in
// [`upgrade_firmware.py`](../../Servomotor/python_programs/upgrade_firmware.py:68).
// WAIT_FOR_RESET_TIME = 0.07 seconds (70ms).
static constexpr uint32_t k_wait_after_pre_reset_ms = 70;

// Local jig behavior: after the last page is sent+ACKed, wait a bit before the
// post-reset, then after the post-reset wait long enough for the bootloader to
// time out and run the application.
static constexpr uint32_t k_wait_before_post_reset_ms = 100;
static constexpr uint32_t k_wait_after_post_reset_ms = 1000;

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

static bool select_sm_firmware_path(String &out_path) {
  out_path = "";

  // Prefer the cached selection (usually set by the web UI).
  const String cached = program_state::servomotor_firmware_filename();
  if (cached.length() > 0) {
    out_path = cached;
    return true;
  }

  // Fall back to persisted active selection (and auto-select if exactly one SM* exists).
  bool auto_sel = false;
  if (!firmware_fs::reconcile_active_servomotor_selection_ex(&out_path, &auto_sel)) {
    Serial.println("ERROR: servomotor firmware not selected (use WiFi UI)");
    return false;
  }

  // Cache it for status/UI.
  program_state::set_servomotor_firmware_filename(out_path);
  return true;
}

bool upgrade_main_firmware_by_unique_id(Servomotor &motor, uint64_t unique_id, const char *firmware_path) {
  if (unique_id == 0) {
    Serial.println("ERROR: unique_id is 0 (invalid)");
    return false;
  }

  if (!firmware_fs::begin()) {
    Serial.println("ERROR: SPIFFS fwfs not mounted");
    return false;
  }

  String path;
  if (firmware_path && firmware_path[0] != 0) {
    path = String(firmware_path);
  } else {
    if (!select_sm_firmware_path(path)) return false;
  }

  File f = SPIFFS.open(path, "r");
  if (!f) {
    Serial.printf("ERROR: could not open servomotor firmware file: %s\n", path.c_str());
    return false;
  }

  const size_t file_size = (size_t)f.size();
  if (file_size < (k_model_code_len + 1u)) {
    Serial.println("ERROR: servomotor firmware file too small");
    f.close();
    return false;
  }

  uint8_t model_code[k_model_code_len];
  uint8_t fw_compat = 0;
  if (!read_all(f, model_code, sizeof(model_code)) || !read_all(f, &fw_compat, 1)) {
    Serial.println("ERROR: failed to read firmware header");
    f.close();
    return false;
  }

  const size_t firmware_data_size = file_size - k_model_code_len - 1u;
  if (firmware_data_size < (k_flash_page_size - 4u)) {
    Serial.printf("ERROR: firmware payload too small (%lu bytes)\n", (unsigned long)firmware_data_size);
    f.close();
    return false;
  }

  // Read the remainder into RAM.
  std::vector<uint8_t> data;
  data.resize(firmware_data_size);
  if (!read_all(f, data.data(), data.size())) {
    Serial.println("ERROR: failed to read firmware payload");
    f.close();
    return false;
  }
  f.close();

  // Pad to multiple-of-4 with 0x00.
  while ((data.size() & 0x3u) != 0u) data.push_back(0x00);
  if (data.size() < 8u) {
    Serial.println("ERROR: firmware payload unexpectedly small after padding");
    return false;
  }

  // Mirror python:
  // firmware_size = (len(data) >> 2) - 1
  // firmware_crc  = crc32(data[4:])
  const uint32_t firmware_size_words = (uint32_t)((data.size() >> 2) - 1u);
  const uint32_t firmware_crc = calculate_crc32(data.data() + 4u, data.size() - 4u);

  // tx = size_words(4 LE) + data[4:] + crc32(4 LE)
  std::vector<uint8_t> tx;
  tx.resize(4u + (data.size() - 4u) + 4u);
  memcpy(tx.data(), &firmware_size_words, 4u);
  memcpy(tx.data() + 4u, data.data() + 4u, data.size() - 4u);
  memcpy(tx.data() + 4u + (data.size() - 4u), &firmware_crc, 4u);

  const size_t max_tx = (size_t)(k_last_firmware_page_number - k_first_firmware_page_number + 1u) * k_flash_page_size;
  if (tx.size() > max_tx) {
    Serial.printf("ERROR: transformed firmware too large (%lu > %lu bytes)\n", (unsigned long)tx.size(),
                  (unsigned long)max_tx);
    return false;
  }

  Serial.printf("Servomotor upgrade: file=%s model='%.8s' compat=%u\n", path.c_str(), (const char *)model_code,
                (unsigned)fw_compat);
  Serial.printf("Servomotor upgrade: tx=%lu bytes size_words=%lu crc32=0x%08lX unique_id=0x%08lX%08lX\n",
                (unsigned long)tx.size(), (unsigned long)firmware_size_words, (unsigned long)firmware_crc,
                (unsigned long)(unique_id >> 32), (unsigned long)(unique_id & 0xFFFFFFFFu));

  // Optional pacing between pages. Default to 0 for unique-ID addressing because
  // each page is ACKed (the ACK provides pacing). If you observe dropped bytes
  // on your RS485 link, raise this (e.g. 50..200ms) instead of modifying the
  // vendored Arduino library.
  static constexpr uint32_t k_inter_page_delay_ms = 0;

  // Reset into bootloader, mirroring the host upgrader flow.
  // Note: this is a *software* reset over RS485, not a hardware NRST toggle.
  Serial.println("Servomotor upgrade: pre-reset (SYSTEM_RESET) ...");
  motor.systemReset();
  {
    const int err = motor.getError();
    if (err != 0) {
      if (err == COMMUNICATION_ERROR_TIMEOUT) {
        Serial.println("ERROR: SYSTEM_RESET timed out (no ACK)");
      } else {
        Serial.printf("ERROR: SYSTEM_RESET failed errno=%d\n", err);
      }
      return false;
    }
  }
  delay(k_wait_after_pre_reset_ms);

  // Send pages and require ACK for each firmwareUpgrade.
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
    motor.firmwareUpgrade(page);
    const int err = motor.getError();
    if (err != 0) {
      if (err == COMMUNICATION_ERROR_TIMEOUT) {
        Serial.printf("ERROR: firmwareUpgrade timed out (no ACK) at page %u\n", (unsigned)page_number);
      } else {
        Serial.printf("ERROR: firmwareUpgrade failed at page %u errno=%d\n", (unsigned)page_number, err);
      }
      return false;
    }

    off += take;
    page_number++;

    if (k_inter_page_delay_ms) delay(k_inter_page_delay_ms);
  }

  delay(k_wait_before_post_reset_ms);

  // Post-reset to start the new firmware (mirrors host upgrader).
  Serial.println("Servomotor upgrade: post-reset (SYSTEM_RESET) ...");
  motor.systemReset();
  {
    const int err = motor.getError();
    if (err != 0) {
      if (err == COMMUNICATION_ERROR_TIMEOUT) {
        Serial.println("ERROR: post SYSTEM_RESET timed out (no ACK)");
      } else {
        Serial.printf("ERROR: post SYSTEM_RESET failed errno=%d\n", err);
      }
      return false;
    }
  }
  delay(k_wait_after_post_reset_ms);

  Serial.println("Servomotor upgrade OK");
  return true;
}

}  // namespace servomotor_upgrade

#undef Serial
