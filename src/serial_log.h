#pragma once

#include <Arduino.h>

#include <FS.h>

namespace serial_log {

// Append-only log in SPIFFS.
//
// Contract:
// - begin() parses existing log and sets in-memory serial_next validity.
// - If no USERSET line exists and log is empty/missing, serial is invalid and
//   production programming must not proceed.
bool begin(fs::FS &fs);

struct SyncResult {
  bool ok = false;
  bool has_last = false;
  bool last_was_userset = false;
  uint32_t last_serial = 0;
  bool has_next = false;
  uint32_t next_serial = 0;
};

// Re-scan the log and re-derive serial_next.
//
// Policy:
// - If last valid line is USERSET_<N>, next_serial=N.
// - Otherwise next_serial=last_serial+1.
//
// This is used by the 's' command and runs automatically at boot.
SyncResult sync_from_log();

bool has_serial_next();
uint32_t serial_next();

// Append USERSET_<serial> and update in-memory serial_next.
bool user_set_serial_next(uint32_t next);

struct Consumed {
  bool valid = false;
  uint32_t serial = 0;
  uint64_t unique_id = 0;
};

// Consume serial after 'e' succeeds.
// - Requires that serial_next is valid.
// - Appends e_<serial>\n.
// - Increments in-memory serial_next to serial+1.
Consumed consume_after_erase();

// For manual 'w' command testing: reserve a serial immediately.
// Appends w_<serial>\n and advances next serial to serial+1.
Consumed reserve_for_write();

// Append summary line with attempted step letters.
// - For success: <steps>_<serial>_OK
// - For failure: <steps>_<serial>_FAIL
bool append_summary(const char *steps, uint32_t serial, bool ok);

// For debug/status printing.
const char *log_path();

}  // namespace serial_log
