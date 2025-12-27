#pragma once

#include <stdbool.h>
#include <stddef.h>

namespace filename_normalizer {

// SPIFFS object name length is limited; in this repo we enforce basename <= 31.
static constexpr size_t k_max_basename_len = 31;

// Normalize an uploaded/host filename into an on-device basename.
//
// Parameters:
// - incoming_filename: may include a path component; both '/' and '\\' are supported.
// - required_prefix: the incoming basename must start with this literal prefix (case-sensitive).
// - replacement_prefix: replaces required_prefix in the output basename.
// - strip_suffix: if non-null and present at the end of the (post-prefix-replaced) name, strip it.
// - strip_suffix_case_insensitive: if true, suffix stripping is case-insensitive; otherwise case-sensitive.
//
// Output constraints:
// - out is NUL-terminated
// - output length must be <= k_max_basename_len
// - output must not contain '/' or '\\'
//
// Returns true on success; on failure returns false and writes a short message to err (if provided).
bool normalize_basename(const char *incoming_filename, const char *required_prefix, const char *replacement_prefix,
                        const char *strip_suffix, bool strip_suffix_case_insensitive, char *out, size_t out_cap,
                        char *err, size_t err_cap);

}  // namespace filename_normalizer

