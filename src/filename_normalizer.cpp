#include "filename_normalizer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace filename_normalizer {

static void set_err(char *err, size_t err_cap, const char *msg) {
  if (!err || err_cap == 0) return;
  if (!msg) msg = "";
  (void)snprintf(err, err_cap, "%s", msg);
}

static const char *basename_ptr(const char *s) {
  if (!s) return "";
  const char *last = s;
  for (const char *p = s; *p; p++) {
    if (*p == '/' || *p == '\\') last = p + 1;
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

static bool ends_with_case_sensitive(const char *s, const char *suffix) {
  if (!s || !suffix) return false;
  const size_t sl = strlen(s);
  const size_t su = strlen(suffix);
  if (sl < su) return false;
  return memcmp(s + (sl - su), suffix, su) == 0;
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

bool normalize_basename(const char *incoming_filename, const char *required_prefix, const char *replacement_prefix,
                        const char *strip_suffix, bool strip_suffix_case_insensitive, char *out, size_t out_cap,
                        char *err, size_t err_cap) {
  if (out && out_cap) out[0] = 0;
  set_err(err, err_cap, "");
  if (!out || out_cap == 0) {
    set_err(err, err_cap, "internal: output buffer missing");
    return false;
  }
  if (!required_prefix || required_prefix[0] == 0) {
    set_err(err, err_cap, "internal: required_prefix missing");
    return false;
  }
  if (!replacement_prefix) replacement_prefix = "";

  const char *base = basename_ptr(incoming_filename);
  if (!starts_with(base, required_prefix)) {
    set_err(err, err_cap, "filename has wrong prefix");
    return false;
  }

  // Build renamed string into a local buffer.
  char tmp[128];
  const char *rest = base + strlen(required_prefix);
  (void)snprintf(tmp, sizeof(tmp), "%s%s", replacement_prefix, rest);

  // Optional suffix stripping.
  if (strip_suffix && strip_suffix[0] != 0) {
    const bool has_suffix = strip_suffix_case_insensitive ? ends_with_case_insensitive(tmp, strip_suffix)
                                                          : ends_with_case_sensitive(tmp, strip_suffix);
    if (has_suffix) {
      tmp[strlen(tmp) - strlen(strip_suffix)] = 0;
    }
  }

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
  if (!starts_with(tmp, replacement_prefix)) {
    set_err(err, err_cap, "normalized filename has wrong output prefix");
    return false;
  }

  if (n + 1 > out_cap) {
    set_err(err, err_cap, "internal: output buffer too small");
    return false;
  }
  memcpy(out, tmp, n + 1);
  return true;
}

}  // namespace filename_normalizer

