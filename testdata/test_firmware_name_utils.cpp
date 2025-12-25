#include "src/firmware_name_utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void ok(const char *in, const char *expect) {
  char out[64];
  char err[96];
  const bool r = firmware_name_utils::normalize_uploaded_firmware_filename(in, out, sizeof(out), err, sizeof(err));
  if (!r) {
    fprintf(stderr, "Expected OK but got error for '%s': %s\n", in, err);
    assert(false);
  }
  assert(strcmp(out, expect) == 0);
}

static void bad(const char *in) {
  char out[64];
  char err[96];
  const bool r = firmware_name_utils::normalize_uploaded_firmware_filename(in, out, sizeof(out), err, sizeof(err));
  if (r) {
    fprintf(stderr, "Expected FAIL but succeeded for '%s' -> '%s'\n", in, out);
    assert(false);
  }
  assert(strlen(err) > 0);
}

int main() {
  ok("bootloader_M17_hw1.5_scc3_1766404965.bin", "BL_M17_hw1.5_scc3_1766404965");
  ok("bootloader_M17_hw1.5_scc3_1766404965.BIN", "BL_M17_hw1.5_scc3_1766404965");
  ok("C:/fakepath/bootloader_M17_hw1.5_scc3_1766404965.bin", "BL_M17_hw1.5_scc3_1766404965");
  ok("\\\\fakepath\\\\bootloader_M17_hw1.5_scc3_1766404965.bin", "BL_M17_hw1.5_scc3_1766404965");
  ok("bootloader_short", "BL_short");

  bad("notbootloader.bin");

  // Too long after normalization (prefix becomes BL but rest stays too long).
  bad("bootloader_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.bin");

  printf("OK\n");
  return 0;
}

