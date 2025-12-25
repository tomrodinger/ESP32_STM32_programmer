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
// NOTE: This is *not* used to drive production serial allocation anymore.
// Production uses the consumed-serial record file as the source of truth to
// avoid serial reuse after crashes.
SyncResult sync_from_log();

bool has_serial_next();
uint32_t serial_next();

// Append USERSET_<serial> and update in-memory serial_next.
bool user_set_serial_next(uint32_t next);

// Append an event line to /log.txt in the form:
//   <TAG>_<value>\n
//
// Intended for audit trails of user/system actions.
//
// Returns true on success.
bool append_event(const char *tag, const char *value);

// Consumed-serial record file (append-only) in SPIFFS.
//
// Format: little-endian uint32_t entries appended back-to-back.
// Special marker: 0x00000000 indicates the sequence has been invalidated
// (followed by the new user-set next serial).
const char *consumed_records_path();

struct RecordsSyncResult {
  bool ok = false;

  bool has_last = false;
  uint32_t last = 0;

  bool has_prev = false;
  uint32_t prev = 0;

  bool has_next = false;
  uint32_t next = 0;

  // True when the last two entries are acceptable for deriving next:
  // - prev == 0 (user-set marker)
  // - OR last == prev + 1
  bool sequence_ok = false;
};

// Re-scan consumed serial record file and derive serial_next.
//
// Policy (per production requirements):
// - If prev==0 (user-set marker), set next=last (no increment).
// - If last two entries are sequential (prev+1==last), set next=last+1.
// - Otherwise invalidate serial_next (production disabled until user sets).
RecordsSyncResult sync_from_consumed_records();

  struct Consumed {
    bool valid = false;
    uint32_t serial = 0;
    uint64_t unique_id = 0;
  };

// Conservative estimate of how many bytes of SPIFFS storage are required per
// successfully programmed unit.
//
// This estimate is intentionally pessimistic so that any derived
// "units_remaining" count will be an under-estimate.
uint32_t bytes_per_unit_estimate();

// Consume the current serial for an upcoming write/program operation.
//
// Contract:
// - Requires that serial_next is valid.
// - Appends the consumed serial to the binary consumed-records file (and closes
//   it) BEFORE the caller begins writing firmware to the target.
// - Advances in-memory serial_next to serial+1.
// - Does NOT append any intermediate marker to /log.txt; production log should
//   contain only the final summary line per unit.
Consumed consume_for_write();

  // Append summary line with attempted step letters.
  // - For success: <steps>_<serial>_<unique_id_hex16>_OK
  // - For failure: <steps>_<serial>_<unique_id_hex16>_FAIL
  //
  // Example:
  //   iewvR_1003_0123456789ABCDEF_OK
  bool append_summary_with_unique_id(const char *steps, uint32_t serial, uint64_t unique_id, bool ok);

  // Legacy format (kept for compatibility with older logs/tools):
  // - For success: <steps>_<serial>_OK
  // - For failure: <steps>_<serial>_FAIL
  bool append_summary(const char *steps, uint32_t serial, bool ok);

// For debug/status printing.
const char *log_path();

}  // namespace serial_log
