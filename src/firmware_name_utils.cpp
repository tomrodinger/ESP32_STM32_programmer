#include "firmware_name_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace firmware_name_utils {

static void set_err(char *err, size_t err_cap, const char *msg) {
  if (!err || err_cap == 0) return;
  if (!msg) msg = "";
  (void)snprintf(err, err_cap, "%s", msg);
}

static const char *basename_ptr(const char *s) {
  if (!s) return "";
  const char *last = s;
  for (const char *p = s; *p; p++) {
    if (*p == '/' || *p == '\\') {
      last = p + 1;
    }
  }
  return last;
}

static bool starts_with(const char *s, const char *prefix) {
  if (!s || !prefix) return false;
  while (*prefix) {
    if (*s++ != *prefix++) return false;
  }
  return true;
}

static bool ends_with_case_insensitive(const char *s, const char *suffix) {
  if (!s || !suffix) return false;
  const size_t sl = strlen(s);
  const size_t su = strlen(suffix);
  if (sl < su) return false;
  const char *tail = s + (sl - su);
  for (size_t i = 0; i < su; i++) {
    const char a = (char)tolower((unsigned char)tail[i]);
    const char b = (char)tolower((unsigned char)suffix[i]);
    if (a != b) return false;
  }
  return true;
}

bool normalize_uploaded_firmware_filename(const char *incoming_filename, char *out, size_t out_cap, char *err,
                                         size_t err_cap) {
  if (out && out_cap) out[0] = 0;
  set_err(err, err_cap, "");
  if (!out || out_cap == 0) {
    set_err(err, err_cap, "internal: output buffer missing");
    return false;
  }

  const char *base = basename_ptr(incoming_filename);
  if (!starts_with(base, "bootloader")) {
    set_err(err, err_cap, "filename must start with 'bootloader'");
    return false;
  }

  // Build the renamed string into a local buffer.
  char tmp[128];
  const char *rest = base + strlen("bootloader");
  (void)snprintf(tmp, sizeof(tmp), "BL%s", rest);

  // Strip trailing .bin (case-insensitive).
  if (ends_with_case_insensitive(tmp, ".bin")) {
    tmp[strlen(tmp) - 4] = 0;
  }

  // Validate and copy.
  const size_t n = strlen(tmp);
  if (n == 0) {
    set_err(err, err_cap, "normalized filename empty");
    return false;
  }
  if (n > k_max_basename_len) {
    set_err(err, err_cap, "normalized filename too long (must be <= 31 chars)");
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (tmp[i] == '/' || tmp[i] == '\\') {
      set_err(err, err_cap, "normalized filename contains '/' ");
      return false;
    }
  }
  if (!starts_with(tmp, "BL")) {
    set_err(err, err_cap, "normalized filename must start with 'BL'");
    return false;
  }

  if (n + 1 > out_cap) {
    set_err(err, err_cap, "internal: output buffer too small");
    return false;
  }
  memcpy(out, tmp, n + 1);
  return true;
}

}  // namespace firmware_name_utils

