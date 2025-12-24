#include "serial_log.h"

#include <SPIFFS.h>

#include <ctype.h>

#include "esp_system.h"  // esp_random()

namespace serial_log {

static constexpr const char *k_log_path = "/log.txt";

// In-memory state guarded by a mutex.
static SemaphoreHandle_t g_mu = nullptr;
static bool g_has_next = false;
static uint32_t g_next = 0;
static fs::FS *g_fs = nullptr;

const char *log_path() { return k_log_path; }

static uint64_t gen_unique_id64() {
  // ESP32 HW RNG.
  const uint64_t hi = (uint64_t)esp_random();
  const uint64_t lo = (uint64_t)esp_random();
  return (hi << 32) | lo;
}

static bool append_line(const char *line) {
  if (!g_fs) return false;
  File f = g_fs->open(k_log_path, FILE_APPEND);
  if (!f) return false;
  const size_t n = strlen(line);
  const size_t w = f.write((const uint8_t *)line, n);
  f.flush();
  f.close();
  return w == n;
}

// Parses a uint32 immediately after the first underscore.
// Accepts formats:
//   PREFIX_<serial>\n
//   PREFIX_<serial>_...\n
// Returns true on success.
static bool parse_serial_after_underscore(const String &line, uint32_t *out) {
  if (!out) return false;
  const int us = line.indexOf('_');
  if (us < 0) return false;
  int i = us + 1;
  if (i >= (int)line.length()) return false;
  // Require at least one digit.
  if (!isDigit(line[i])) return false;

  uint64_t v = 0;
  while (i < (int)line.length()) {
    const char c = line[i];
    if (!isDigit(c)) break;
    v = v * 10u + (uint32_t)(c - '0');
    if (v > 0xFFFFFFFFull) return false;
    i++;
  }
  *out = (uint32_t)v;
  return true;
}

static SyncResult scan_file_for_last_serial() {
  SyncResult res;
  if (!g_fs) return res;

  res.ok = true;

  if (!g_fs->exists(k_log_path)) {
    // No log is not an error; simply means no serial is known.
    return res;
  }

  File f = g_fs->open(k_log_path, "r");
  if (!f) {
    res.ok = false;
    return res;
  }

  String cur;
  uint8_t buf[256];
  while (true) {
    const int r = f.read(buf, sizeof(buf));
    if (r <= 0) break;
    for (int bi = 0; bi < r; bi++) {
      const char c = (char)buf[bi];
      if (c == '\n') {
        uint32_t s = 0;
        const bool ok = parse_serial_after_underscore(cur, &s);
        if (ok) {
          res.has_last = true;
          res.last_serial = s;
          res.last_was_userset = cur.startsWith("USERSET_");
        }
        cur = "";
      } else if (c != '\r') {
        cur += c;
      }
    }
  }

  f.close();
  return res;
}

static void set_state(bool has, uint32_t next) {
  if (!g_mu) g_mu = xSemaphoreCreateMutex();
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_has_next = has;
  g_next = next;
  xSemaphoreGive(g_mu);
}

bool begin(fs::FS &fs) {
  g_fs = &fs;
  if (!g_mu) g_mu = xSemaphoreCreateMutex();

  // Default: invalid until derived.
  set_state(false, 0);

  const SyncResult s = sync_from_log();
  return s.ok;
}

SyncResult sync_from_log() {
  SyncResult res = scan_file_for_last_serial();
  if (!res.ok) {
    set_state(false, 0);
    return res;
  }

  if (!res.has_last) {
    set_state(false, 0);
    return res;
  }

  // Per user request for 's': normal increment (+1) unless explicit USERSET.
  const uint32_t next = res.last_was_userset ? res.last_serial : (res.last_serial + 1u);
  res.has_next = true;
  res.next_serial = next;
  set_state(true, next);
  return res;
}

bool has_serial_next() {
  if (!g_mu) return false;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool has = g_has_next;
  xSemaphoreGive(g_mu);
  return has;
}

uint32_t serial_next() {
  if (!g_mu) return 0;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  const uint32_t v = g_next;
  xSemaphoreGive(g_mu);
  return v;
}

bool user_set_serial_next(uint32_t next) {
  // Append-only: do not rewrite.
  char line[64];
  const int n = snprintf(line, sizeof(line), "USERSET_%lu\n", (unsigned long)next);
  if (n <= 0 || (size_t)n >= sizeof(line)) return false;
  if (!append_line(line)) return false;
  set_state(true, next);
  return true;
}

Consumed consume_after_erase() {
  Consumed out;
  if (!g_mu) return out;

  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool has = g_has_next;
  const uint32_t next = g_next;
  xSemaphoreGive(g_mu);
  if (!has) return out;

  // Reserve/consume.
  const uint32_t consumed = next;
  char line[32];
  const int n = snprintf(line, sizeof(line), "e_%lu\n", (unsigned long)consumed);
  if (n <= 0 || (size_t)n >= sizeof(line)) return out;
  if (!append_line(line)) return out;

  // Update in-memory state for next unit.
  set_state(true, consumed + 1u);

  out.valid = true;
  out.serial = consumed;
  out.unique_id = gen_unique_id64();
  return out;
}

Consumed reserve_for_write() {
  Consumed out;
  if (!g_mu) return out;

  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool has = g_has_next;
  const uint32_t next = g_next;
  xSemaphoreGive(g_mu);
  if (!has) return out;

  const uint32_t consumed = next;
  char line[32];
  const int n = snprintf(line, sizeof(line), "w_%lu\n", (unsigned long)consumed);
  if (n <= 0 || (size_t)n >= sizeof(line)) return out;
  if (!append_line(line)) return out;

  set_state(true, consumed + 1u);

  out.valid = true;
  out.serial = consumed;
  out.unique_id = gen_unique_id64();
  return out;
}

bool append_summary(const char *steps, uint32_t serial, bool ok) {
  if (!steps) return false;
  char line[80];
  const int n = snprintf(line, sizeof(line), "%s_%lu_%s\n", steps, (unsigned long)serial, ok ? "OK" : "FAIL");
  if (n <= 0 || (size_t)n >= sizeof(line)) return false;
  return append_line(line);
}

}  // namespace serial_log
