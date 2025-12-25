#include "firmware_fs.h"

#include "firmware_name_utils.h"

#include <FS.h>
#include <SPIFFS.h>

#include <ctype.h>

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

static bool ends_with(const char *s, const char *suffix) {
  if (!s || !suffix) return false;
  const size_t sl = strlen(s);
  const size_t su = strlen(suffix);
  if (sl < su) return false;
  return memcmp(s + (sl - su), suffix, su) == 0;
}

static bool ends_with_case_insensitive(const String &s, const char *suffix) {
  if (!suffix) return false;
  const size_t su = strlen(suffix);
  if (s.length() < su) return false;
  const size_t start = s.length() - su;
  for (size_t i = 0; i < su; i++) {
    const char a = (char)tolower((unsigned char)s[start + i]);
    const char b = (char)tolower((unsigned char)suffix[i]);
    if (a != b) return false;
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
  if (out_count) *out_count = 0;

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
    if (!starts_with(base, "BL")) continue;
    if (n < out_cap && out) {
      out[n] = String(base);
    }
    n++;
  }

  if (out_count) *out_count = n;
  return true;
}

static bool basename_is_valid(const String &b) {
  if (b.length() == 0) return false;
  if (b.length() > k_max_firmware_basename_len) return false;
  if (b.indexOf('/') >= 0) return false;
  if (!b.startsWith("BL")) return false;
  return true;
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

bool set_active_firmware_basename(const String &basename) {
  if (!basename_is_valid(basename)) return false;

  // Must refer to an existing file.
  const String path = String("/") + basename;
  if (!SPIFFS.exists(path)) return false;

  File f = SPIFFS.open(active_firmware_selection_path(), "w");
  if (!f) return false;
  const size_t w = f.print(basename);
  f.print("\n");
  f.flush();
  f.close();
  return w == basename.length();
}

bool clear_active_firmware_selection() {
  if (!SPIFFS.exists(active_firmware_selection_path())) return true;
  return SPIFFS.remove(active_firmware_selection_path());
}

static bool read_active_basename(String &out_basename) {
  out_basename = "";
  File f = SPIFFS.open(active_firmware_selection_path(), "r");
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  line.replace("\r", "");
  line.trim();
  if (!basename_is_valid(line)) return false;
  out_basename = line;
  return true;
}

bool get_active_firmware_path(String &out_path) {
  out_path = "";

  String sel;
  if (read_active_basename(sel)) {
    const String p = String("/") + sel;
    if (SPIFFS.exists(p)) {
      out_path = p;
      return true;
    }
  }

  // Auto-select if exactly one BL* file exists.
  String tmp[2];
  size_t count = 0;
  if (!list_firmware_basenames(tmp, 2, &count)) return false;
  if (count == 1) {
    const String only = tmp[0];
    if (set_active_firmware_basename(only)) {
      out_path = String("/") + only;
      return true;
    }
    return false;
  }

  return false;
}

bool reconcile_active_selection_ex(String *out_active_path, bool *out_auto_selected) {
  if (out_auto_selected) *out_auto_selected = false;

  // Detect whether we had a valid persisted selection before.
  String before;
  bool had_valid_before = false;
  if (read_active_basename(before)) {
    const String p = String("/") + before;
    had_valid_before = SPIFFS.exists(p);
  }

  String p;
  const bool ok = get_active_firmware_path(p);
  if (out_active_path) *out_active_path = ok ? p : String("");
  if (ok) {
    // If we didn't have a valid selection before, but we do now, this must
    // have been an auto-select due to "exactly one BL*".
    if (out_auto_selected && !had_valid_before && p.length() > 1) {
      *out_auto_selected = true;
    }
    return true;
  }

  // If selection file exists but points nowhere, clear it (avoid misleading UI).
  String sel;
  if (SPIFFS.exists(active_firmware_selection_path())) {
    if (read_active_basename(sel)) {
      const String fp = String("/") + sel;
      if (!SPIFFS.exists(fp)) {
        (void)clear_active_firmware_selection();
      }
    } else {
      // Corrupt/invalid selection file.
      (void)clear_active_firmware_selection();
    }
  }
  return false;
}

bool reconcile_active_selection(String *out_active_path) {
  return reconcile_active_selection_ex(out_active_path, nullptr);
}

}  // namespace firmware_fs
