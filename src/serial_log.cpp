#include "serial_log.h"

#include <SPIFFS.h>

#include <ctype.h>

#include "esp_system.h"  // esp_random()

namespace serial_log {

static constexpr const char *k_log_path = "/log.txt";
static constexpr const char *k_consumed_records_path = "/serial_consumed.bin";

static constexpr uint32_t k_unique_id_hex_len = 16u;

// In-memory state guarded by a mutex.
static SemaphoreHandle_t g_mu = nullptr;
static bool g_has_next = false;
static uint32_t g_next = 0;
static fs::FS *g_fs = nullptr;

const char *log_path() { return k_log_path; }

const char *consumed_records_path() { return k_consumed_records_path; }

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

uint32_t bytes_per_unit_estimate() {
  // Estimate how many SPIFFS bytes are consumed per unit attempt.
  //
  // We want to under-estimate units remaining, so this estimate is slightly
  // pessimistic, but still simple and representative.
  //
  // Successful/failure log line format:
  //   <steps>_<serial>_<unique_id_hex16>_OK\n
  // Additionally, before programming we append one uint32_t to
  // /serial_consumed.bin (4 bytes).

  // Representative (slightly pessimistic) example line.
  static constexpr const char *k_example_line = "iewvR_99999_0123456789ABCDEF_OK\n";
  const uint32_t log_line_bytes = (uint32_t)strlen(k_example_line);

  // One consumed-serial record entry (little-endian uint32).
  static constexpr uint32_t k_consumed_record_bytes = 4u;

  // Small slack for SPIFFS allocation granularity / minor future expansion.
  static constexpr uint32_t k_overhead_bytes = 16u;

  return log_line_bytes + k_consumed_record_bytes + k_overhead_bytes;
}

static bool append_consumed_u32(uint32_t v) {
  if (!g_fs) return false;
  File f = g_fs->open(k_consumed_records_path, FILE_APPEND);
  if (!f) return false;

  // Stored as little-endian uint32_t for compactness.
  uint8_t b[4];
  b[0] = (uint8_t)(v & 0xFFu);
  b[1] = (uint8_t)((v >> 8) & 0xFFu);
  b[2] = (uint8_t)((v >> 16) & 0xFFu);
  b[3] = (uint8_t)((v >> 24) & 0xFFu);

  const size_t w = f.write(b, sizeof(b));
  f.flush();
  f.close();
  return w == sizeof(b);
}

static bool pad_consumed_records_to_u32_boundary_with_zeros() {
  if (!g_fs) return false;
  if (!g_fs->exists(k_consumed_records_path)) return true;

  File f = g_fs->open(k_consumed_records_path, "r");
  if (!f) return false;
  const size_t sz = (size_t)f.size();
  f.close();

  const size_t rem = sz % 4u;
  if (rem == 0u) return true;

  // Append 0x00 bytes until size is a multiple of 4.
  File fa = g_fs->open(k_consumed_records_path, FILE_APPEND);
  if (!fa) return false;
  const size_t need = 4u - rem;
  for (size_t i = 0; i < need; i++) {
    const uint8_t z = 0u;
    if (fa.write(&z, 1) != 1) {
      fa.close();
      return false;
    }
  }
  fa.flush();
  fa.close();
  return true;
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

  // Best-effort parse of /log.txt (debug/audit only).
  const SyncResult s = sync_from_log();

  // If consumed-records exists, it becomes the source of truth for whether we
  // can safely continue production without serial reuse.
  const RecordsSyncResult r = sync_from_consumed_records();

  return s.ok && r.ok;
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

  // Historical policy: normal increment (+1) unless explicit USERSET.
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
  // 1) Invalidate the consumed-records sequence and seed it with the new value.
  //    This is used on reboot to allow resuming without serial reuse.
  // If the file is corrupted (size not multiple of 4), pad with zeros until it
  // becomes a multiple of 4, then append the invalidation marker.
  if (!pad_consumed_records_to_u32_boundary_with_zeros()) return false;
  if (!append_consumed_u32(0x00000000u)) return false;
  if (!append_consumed_u32(next)) return false;

  // 2) Append a human-readable marker to /log.txt.
  char line[64];
  const int n = snprintf(line, sizeof(line), "USERSET_%lu\n", (unsigned long)next);
  if (n <= 0 || (size_t)n >= sizeof(line)) return false;
  if (!append_line(line)) return false;

  set_state(true, next);
  return true;
}

RecordsSyncResult sync_from_consumed_records() {
  RecordsSyncResult res;
  if (!g_fs) return res;
  res.ok = true;

  if (!g_fs->exists(k_consumed_records_path)) {
    // No records file yet: production must be disabled until user sets serial
    // (which seeds the records file with: 0x00000000, <serial_next>).
    set_state(false, 0);
    return res;
  }

  File f = g_fs->open(k_consumed_records_path, "r");
  if (!f) {
    res.ok = false;
    return res;
  }

  const size_t sz = (size_t)f.size();
  if ((sz % 4) != 0) {
    // Corrupt/partial write: refuse to continue until user sets serial.
    f.close();
    set_state(false, 0);
    return res;
  }

  if (sz < 8) {
    // Size 0 or 4 means we can't validate sequencing. Require user-set serial.
    f.close();
    set_state(false, 0);
    return res;
  }

  auto read_u32_le_at = [&](size_t off, uint32_t *out) -> bool {
    if (!out) return false;
    if (!f.seek((uint32_t)off, SeekSet)) return false;
    uint8_t b[4];
    const int r = f.read(b, sizeof(b));
    if (r != (int)sizeof(b)) return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
  };

  uint32_t prev = 0;
  uint32_t last = 0;
  const bool ok_prev = read_u32_le_at(sz - 8, &prev);
  const bool ok_last = read_u32_le_at(sz - 4, &last);
  f.close();

  if (!ok_prev || !ok_last) {
    set_state(false, 0);
    return res;
  }

  res.has_prev = true;
  res.prev = prev;
  res.has_last = true;
  res.last = last;

  if (prev == 0x00000000u) {
    // User-set marker: accept last as the next serial (no increment).
    res.sequence_ok = true;
    res.has_next = true;
    res.next = last;
    set_state(true, last);
    return res;
  }

  if (last == (uint32_t)(prev + 1u)) {
    res.sequence_ok = true;
    res.has_next = true;
    res.next = last + 1u;
    set_state(true, last + 1u);
    return res;
  }

  // Non-sequential: refuse to continue until user sets serial via UI.
  res.sequence_ok = false;
  set_state(false, 0);
  return res;
}

Consumed consume_for_write() {
  Consumed out;
  if (!g_mu) return out;

  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool has = g_has_next;
  const uint32_t next = g_next;
  xSemaphoreGive(g_mu);
  if (!has) return out;

  // Must persist (append-only) before the caller begins programming the target.
  if (!append_consumed_u32(next)) return out;

  set_state(true, next + 1u);

  out.valid = true;
  out.serial = next;
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

bool append_summary_with_unique_id(const char *steps, uint32_t serial, uint64_t unique_id, bool ok) {
  if (!steps) return false;

  // Format unique_id as fixed-width 16 hex characters (upper-case) without 0x prefix.
  // Print using two 32-bit chunks to avoid %llX portability surprises on embedded libc.
  const uint32_t hi = (uint32_t)(unique_id >> 32);
  const uint32_t lo = (uint32_t)(unique_id & 0xFFFFFFFFu);

  // Size: steps(<=16) + '_' + serial(<=10) + '_' + 16 + '_' + FAIL + '\n' + NUL
  char line[96];
  const int n = snprintf(line, sizeof(line), "%s_%lu_%08lX%08lX_%s\n", steps, (unsigned long)serial, (unsigned long)hi,
                         (unsigned long)lo, ok ? "OK" : "FAIL");
  if (n <= 0 || (size_t)n >= sizeof(line)) return false;
  return append_line(line);
}

}  // namespace serial_log
