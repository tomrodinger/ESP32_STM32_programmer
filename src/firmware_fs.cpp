#include "firmware_fs.h"

#include <FS.h>
#include <SPIFFS.h>

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

bool find_single_firmware_bin(String &out_path) {
  out_path = "";

  // Enumerate root directory. Note this is relative to the mountpoint.
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("ERROR: filesystem root not accessible (is SPIFFS mounted?)");
    return false;
  }

  String found_name;
  uint32_t matches = 0;

  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) continue;

    const char *name = f.name();
    // In Arduino FS, name often includes the mountpoint prefix.
    // We accept both "bootloader*.bin" at any path segment, and also a full path.
    // Deterministic policy: exactly one match must exist.

    // Extract basename for matching.
    const char *base = strrchr(name, '/');
    base = base ? (base + 1) : name;

    // SPIFFS filename length is limited (mkspiffs in this toolchain reports
    // SPIFFS_OBJ_NAME_LEN=32). We therefore use a short prefix and allow the
    // binary file to be extensionless.
    if (!starts_with(base, "BL")) continue;

    matches++;
    // Normalize to a path usable by Arduino FS open() (must start with '/').
    // f.name() is typically already like "/bootloader.bin".
    found_name = String(name);
    if (!found_name.startsWith("/")) {
      found_name = String("/") + found_name;
    }
  }

  if (matches == 0) {
    Serial.println("ERROR: no firmware file found matching pattern BL*");
    return false;
  }
  if (matches > 1) {
    Serial.printf("ERROR: multiple firmware files found matching pattern BL* (%lu matches). Remove extras.\n",
                  (unsigned long)matches);
    return false;
  }

  out_path = found_name;
  Serial.printf("Selected firmware file: %s\n", out_path.c_str());
  return true;
}

}  // namespace firmware_fs
