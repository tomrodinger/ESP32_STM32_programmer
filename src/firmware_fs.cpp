#include "firmware_fs.h"

#include "firmware_name_utils.h"
#include "filename_normalizer.h"

#include <FS.h>
#include <SPIFFS.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "tee_log.h"

// Route all Serial prints in this file into the RAM terminal buffer as well.
#define Serial tee_log::out()

namespace firmware_fs {

// Note: SPIFFS is mounted at base path "/spiffs", but Arduino's FS APIs
// expect *paths relative to the mountpoint* (i.e. "/foo.bin", not "/littlefs/foo.bin").
// Internally, the VFS mountpoint is prepended.
static constexpr const char *k_base_path = "/spiffs";
static constexpr const char *k_partition_label = "fwfs";

static bool starts_with(const char *s, const char *prefix) {
  if (!s || !prefix) return false;
  while (*prefix) {
    if (*s++ != *prefix++) return false;
  }
  return true;
}

const char *active_firmware_selection_path() { return "/active_firmware.txt"; }

bool begin() {
  // Note: Arduino-ESP32 SPIFFS defaults partitionLabel="spiffs".
  // We explicitly mount partitionLabel="fwfs" (see custom partitions CSV).
  return SPIFFS.begin(/*formatOnFail=*/false, k_base_path, /*maxOpenFiles=*/10, k_partition_label);
}

static void list_dir(fs::FS &fs, const char *dirname) {
  File root = fs.open(dirname);
  if (!root) {
    Serial.printf("FS: failed to open dir %s\n", dirname);
    return;
  }
  if (!root.isDirectory()) {
    Serial.printf("FS: %s is not a directory\n", dirname);
    return;
  }

  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    Serial.printf("FS: %s (%s) %lu bytes\n", f.name(), f.isDirectory() ? "dir" : "file",
                  (unsigned long)f.size());
  }
}

void print_status() {
  Serial.println("Filesystem status:");
  Serial.printf("  base path (mountpoint): %s\n", k_base_path);
  Serial.printf("  partition label: %s\n", k_partition_label);
  Serial.printf("  totalBytes=%lu usedBytes=%lu\n", (unsigned long)SPIFFS.totalBytes(), (unsigned long)SPIFFS.usedBytes());
  // Root directory in Arduino FS is "/" (relative to mountpoint).
  list_dir(SPIFFS, "/");
}

bool list_firmware_basenames(String *out, size_t out_cap, size_t *out_count) {
  return list_basenames(FileKind::kBootloader, out, out_cap, out_count);
}

bool list_servomotor_firmware_basenames(String *out, size_t out_cap, size_t *out_count) {
  return list_basenames(FileKind::kServomotorFirmware, out, out_cap, out_count);
}

bool list_basenames(FileKind kind, String *out, size_t out_cap, size_t *out_count) {
  if (out_count) *out_count = 0;

  const char *prefix = nullptr;
  switch (kind) {
    case FileKind::kBootloader:
      prefix = "BL";
      break;
    case FileKind::kServomotorFirmware:
      prefix = "SM";
      break;
    default:
      return false;
  }

  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("ERROR: filesystem root not accessible (is SPIFFS mounted?)");
    return false;
  }

  size_t n = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) continue;
    const char *name = f.name();
    const char *base = strrchr(name, '/');
    base = base ? (base + 1) : name;
    if (!starts_with(base, prefix)) continue;
    if (n < out_cap && out) {
      out[n] = String(base);
    }
    n++;
  }

  if (out_count) *out_count = n;
  return true;
}

static bool basename_is_valid_by_kind(FileKind kind, const String &b) {
  if (b.length() == 0) return false;
  if (b.length() > k_max_firmware_basename_len) return false;
  if (b.indexOf('/') >= 0) return false;
  switch (kind) {
    case FileKind::kBootloader:
      return b.startsWith("BL");
    case FileKind::kServomotorFirmware:
      return b.startsWith("SM");
    default:
      return false;
  }
}

bool normalize_uploaded_firmware_filename(const String &incoming_filename, String &out_basename, String *out_err) {
  out_basename = "";
  if (out_err) *out_err = "";

  char out[firmware_name_utils::k_max_basename_len + 1];
  char err[96];
  const bool ok = firmware_name_utils::normalize_uploaded_firmware_filename(
      incoming_filename.c_str(), out, sizeof(out), err, sizeof(err));
  if (!ok) {
    if (out_err) *out_err = String(err);
    return false;
  }
  out_basename = String(out);
  return true;
}

bool normalize_uploaded_filename(FileKind kind, const String &incoming_filename, String &out_basename, String *out_err) {
  out_basename = "";
  if (out_err) *out_err = "";

  if (kind == FileKind::kBootloader) {
    return normalize_uploaded_firmware_filename(incoming_filename, out_basename, out_err);
  }

  // For everything else, use the generic normalizer.
  const char *required_prefix = "";
  const char *replacement_prefix = "";
  const char *strip_suffix = "";
  bool suffix_ci = false;

  switch (kind) {
    case FileKind::kServomotorFirmware:
      // Accept either a host-style name starting with "servomotor" or an already-normalized on-device-style
      // name starting with "SM".
      required_prefix = "servomotor";
      replacement_prefix = "SM";
      strip_suffix = ".firmware";
      suffix_ci = false;  // case-sensitive.
      break;
    default:
      if (out_err) *out_err = "internal: unknown file kind";
      return false;
  }

  char out[firmware_name_utils::k_max_basename_len + 1];
  char err[96];
  bool ok = filename_normalizer::normalize_basename(incoming_filename.c_str(), required_prefix, replacement_prefix, strip_suffix,
                                                    suffix_ci, out, sizeof(out), err, sizeof(err));
  if (!ok && kind == FileKind::kServomotorFirmware && strcmp(err, "filename has wrong prefix") == 0) {
    // Retry with already-normalized prefix.
    ok = filename_normalizer::normalize_basename(incoming_filename.c_str(), "SM", "SM", strip_suffix, suffix_ci, out,
                                                 sizeof(out), err, sizeof(err));
  }
  if (!ok) {
    if (out_err) {
      // Improve the most common error string to match the bootloader UX.
      if (strcmp(err, "filename has wrong prefix") == 0) {
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "filename must start with '%s'", required_prefix);
        *out_err = String(msg);
      } else {
        *out_err = String(err);
      }
    }
    return false;
  }

  out_basename = String(out);
  return true;
}

bool set_active_firmware_basename(const String &basename) {
  return set_active_basename(FileKind::kBootloader, basename);
}

static const char *active_selection_path_by_kind(FileKind kind) {
  switch (kind) {
    case FileKind::kBootloader:
      return active_firmware_selection_path();
    case FileKind::kServomotorFirmware:
      return "/active_servomotor_firmware.txt";
    default:
      return nullptr;
  }
}

bool set_active_basename(FileKind kind, const String &basename) {
  const char *sel_path = active_selection_path_by_kind(kind);
  if (!sel_path) return false;
  if (!basename_is_valid_by_kind(kind, basename)) return false;

  const String path = String("/") + basename;
  if (!SPIFFS.exists(path)) return false;

  File f = SPIFFS.open(sel_path, "w");
  if (!f) return false;
  const size_t w = f.print(basename);
  f.print("\n");
  f.flush();
  f.close();
  return w == basename.length();
}

bool clear_active_firmware_selection() {
  return clear_active_selection(FileKind::kBootloader);
}

bool clear_active_selection(FileKind kind) {
  const char *sel_path = active_selection_path_by_kind(kind);
  if (!sel_path) return false;
  if (!SPIFFS.exists(sel_path)) return true;
  return SPIFFS.remove(sel_path);
}

static bool read_active_basename_by_kind(FileKind kind, String &out_basename);

static bool read_active_basename(String &out_basename) {
  return read_active_basename_by_kind(FileKind::kBootloader, out_basename);
}

static bool read_active_basename_by_kind(FileKind kind, String &out_basename) {
  out_basename = "";
  const char *sel_path = active_selection_path_by_kind(kind);
  if (!sel_path) return false;

  File f = SPIFFS.open(sel_path, "r");
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  line.replace("\r", "");
  line.trim();
  if (!basename_is_valid_by_kind(kind, line)) return false;
  out_basename = line;
  return true;
}

bool get_active_firmware_path(String &out_path) {
  return get_active_path(FileKind::kBootloader, out_path);
}

bool get_active_path(FileKind kind, String &out_path) {
  out_path = "";

  String sel;
  if (read_active_basename_by_kind(kind, sel)) {
    const String p = String("/") + sel;
    if (SPIFFS.exists(p)) {
      out_path = p;
      return true;
    }
  }

  // Auto-select if exactly one matching file exists.
  String tmp[2];
  size_t count = 0;
  if (!list_basenames(kind, tmp, 2, &count)) return false;
  if (count == 1) {
    const String only = tmp[0];
    if (set_active_basename(kind, only)) {
      out_path = String("/") + only;
      return true;
    }
    return false;
  }

  return false;
}

bool reconcile_active_selection_ex(String *out_active_path, bool *out_auto_selected) {
  return reconcile_active_selection_ex(FileKind::kBootloader, out_active_path, out_auto_selected);
}

bool reconcile_active_selection_ex(FileKind kind, String *out_active_path, bool *out_auto_selected) {
  if (out_auto_selected) *out_auto_selected = false;

  const char *sel_path = active_selection_path_by_kind(kind);
  if (!sel_path) {
    if (out_active_path) *out_active_path = String("");
    return false;
  }

  // Detect whether we had a valid persisted selection before.
  String before;
  bool had_valid_before = false;
  if (read_active_basename_by_kind(kind, before)) {
    const String p = String("/") + before;
    had_valid_before = SPIFFS.exists(p);
  }

  String p;
  const bool ok = get_active_path(kind, p);
  if (out_active_path) *out_active_path = ok ? p : String("");
  if (ok) {
    if (out_auto_selected && !had_valid_before && p.length() > 1) {
      *out_auto_selected = true;
    }
    return true;
  }

  // If selection file exists but points nowhere (or is invalid), clear it.
  String sel;
  if (SPIFFS.exists(sel_path)) {
    if (read_active_basename_by_kind(kind, sel)) {
      const String fp = String("/") + sel;
      if (!SPIFFS.exists(fp)) {
        (void)clear_active_selection(kind);
      }
    } else {
      (void)clear_active_selection(kind);
    }
  }
  return false;
}

bool get_active_servomotor_firmware_path(String &out_path) {
  return get_active_path(FileKind::kServomotorFirmware, out_path);
}

bool reconcile_active_servomotor_selection_ex(String *out_active_path, bool *out_auto_selected) {
  return reconcile_active_selection_ex(FileKind::kServomotorFirmware, out_active_path, out_auto_selected);
}

bool reconcile_active_selection(String *out_active_path) {
  return reconcile_active_selection_ex(out_active_path, nullptr);
}

}  // namespace firmware_fs

#undef Serial
