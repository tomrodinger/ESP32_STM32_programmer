#include "firmware_name_utils.h"

#include "filename_normalizer.h"

#include <stdio.h>
#include <string.h>

namespace firmware_name_utils {

bool normalize_uploaded_firmware_filename(const char *incoming_filename, char *out, size_t out_cap, char *err,
                                          size_t err_cap) {
  // Backwards-compatible wrapper around the generic filename normalizer.
  // Requirements for bootloader images:
  // - incoming starts with "bootloader"
  // - replace with "BL"
  // - strip ".bin" (case-insensitive)
  // - ensure <= 31 chars
  //
  // NOTE: the generic normalizer returns a generic error string; callers that
  // want more specific messaging can implement their own wrapper.
  const bool ok = filename_normalizer::normalize_basename(incoming_filename, "bootloader", "BL", ".bin",
                                                          /*strip_suffix_case_insensitive=*/true, out, out_cap, err,
                                                          err_cap);
  if (!ok && err && err_cap) {
    // Preserve historical error string for the most common failure.
    // (Keeps tests/UI expectations stable.)
    // If the failure is not the prefix mismatch, keep the generic message.
    if (strcmp(err, "filename has wrong prefix") == 0) {
      (void)snprintf(err, err_cap, "%s", "filename must start with 'bootloader'");
    }
  }
  return ok;
}

}  // namespace firmware_name_utils
