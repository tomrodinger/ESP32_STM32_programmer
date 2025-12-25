#pragma once

#include <stddef.h>

namespace firmware_name_utils {

// SPIFFS object name length is limited; in this repo we enforce basename <= 31.
static constexpr size_t k_max_basename_len = 31;

// Normalize an uploaded firmware filename into an on-device basename.
//
// Rules:
// - Keep only the basename (strip any path component, supporting both '/' and '\\').
// - Incoming basename must start with literal prefix "bootloader" (case-sensitive).
// - Replace leading "bootloader" with "BL".
// - Strip trailing ".bin" extension (case-insensitive) if present.
// - Output must be <= k_max_basename_len and contain no '/' or '\\'.
//
// Returns true on success and writes a NUL-terminated basename to out.
// On failure, returns false and writes a short error string to err (if provided).
bool normalize_uploaded_firmware_filename(const char *incoming_filename, char *out, size_t out_cap, char *err,
                                         size_t err_cap);

}  // namespace firmware_name_utils

