#pragma once

#include <Arduino.h>

namespace firmware_fs {

// SPIFFS object name length is limited (mkspiffs in this toolchain reports
// SPIFFS_OBJ_NAME_LEN=32 including null terminator). We therefore enforce a
// maximum basename length of 31 so that "/" + basename fits.
static constexpr size_t k_max_firmware_basename_len = 31;

// Mount SPIFFS at base path "/spiffs" using partition label "fwfs".
// Returns true on success.
bool begin();

// Prints basic FS stats + lists all files in the root directory.
void print_status();

// Enumerate firmware files (basenames, no leading "/") matching the pattern "BL*".
// Returns true if the filesystem root is accessible (even if list is empty).
bool list_firmware_basenames(String *out, size_t out_cap, size_t *out_count);

// Location (within SPIFFS) where we persist the active firmware selection.
// The file contains a single line: the selected firmware basename (no leading "/").
const char *active_firmware_selection_path();

// Normalize an uploaded firmware filename into an on-device basename.
// Rules:
// - Must start with "bootloader" (case-sensitive).
// - Rename by replacing leading "bootloader" with "BL".
// - Strip a trailing ".bin" extension (case-insensitive) if present.
// - Result must be <= k_max_firmware_basename_len and contain no '/'.
bool normalize_uploaded_firmware_filename(const String &incoming_filename, String &out_basename, String *out_err);

// Persist active firmware selection by basename (no leading "/").
// Returns false if name is invalid or cannot be written.
bool set_active_firmware_basename(const String &basename);

// Clear persisted active selection (if any). Returns true on success.
bool clear_active_firmware_selection();

// Determine active firmware file path (leading "/").
// State machine:
// - If a persisted active selection exists and points to an existing BL* file, return it.
// - Else, if there is exactly one BL* file, auto-select it (persist) and return it.
// - Else return false (unselected; programming disabled).
bool get_active_firmware_path(String &out_path);

// Ensure the persisted selection is consistent with filesystem contents.
// Returns true if, after reconciliation, an active selection exists.
bool reconcile_active_selection(String *out_active_path);

// Same as reconcile_active_selection(), but indicates whether selection was made
// automatically during this call.
//
// If out_auto_selected is non-null, it is set to true only when the function
// auto-selected and persisted a firmware file due to "exactly one BL*" policy.
bool reconcile_active_selection_ex(String *out_active_path, bool *out_auto_selected);

}  // namespace firmware_fs
