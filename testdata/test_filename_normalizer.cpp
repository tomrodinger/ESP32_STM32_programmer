#include "src/filename_normalizer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void ok(const char *in, const char *req_prefix, const char *rep_prefix, const char *strip_suffix,
               bool suffix_ci, const char *expect) {
  char out[64];
  char err[96];
  const bool r = filename_normalizer::normalize_basename(in, req_prefix, rep_prefix, strip_suffix, suffix_ci, out,
                                                         sizeof(out), err, sizeof(err));
  if (!r) {
    fprintf(stderr, "Expected OK but got error for '%s': %s\n", in, err);
    assert(false);
  }
  assert(strcmp(out, expect) == 0);
}

static void bad(const char *in, const char *req_prefix, const char *rep_prefix, const char *strip_suffix,
                bool suffix_ci) {
  char out[64];
  char err[96];
  const bool r = filename_normalizer::normalize_basename(in, req_prefix, rep_prefix, strip_suffix, suffix_ci, out,
                                                         sizeof(out), err, sizeof(err));
  if (r) {
    fprintf(stderr, "Expected FAIL but succeeded for '%s' -> '%s'\n", in, out);
    assert(false);
  }
  assert(strlen(err) > 0);
}

int main() {
  // Bootloader style: bootloader* -> BL* and strip .bin case-insensitive.
  ok("bootloader_M17_hw1.5_scc3_1766404965.bin", "bootloader", "BL", ".bin", true, "BL_M17_hw1.5_scc3_1766404965");
  ok("bootloader_M17_hw1.5_scc3_1766404965.BIN", "bootloader", "BL", ".bin", true, "BL_M17_hw1.5_scc3_1766404965");
  ok("C:/fakepath/bootloader_M17_hw1.5_scc3_1766404965.bin", "bootloader", "BL", ".bin", true,
     "BL_M17_hw1.5_scc3_1766404965");
  ok("\\\\fakepath\\\\bootloader_M17_hw1.5_scc3_1766404965.bin", "bootloader", "BL", ".bin", true,
     "BL_M17_hw1.5_scc3_1766404965");

  bad("notbootloader.bin", "bootloader", "BL", ".bin", true);
  bad("bootloader_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.bin", "bootloader", "BL", ".bin", true);

  // Servomotor style: servomotor* -> SM* and strip .firmware case-sensitive.
  ok("servomotor_M17_fw0.14.0.0_scc3_hw1.5.firmware", "servomotor", "SM", ".firmware", false,
     "SM_M17_fw0.14.0.0_scc3_hw1.5");
  ok("C:/fakepath/servomotor_M17_fw0.14.0.0_scc3_hw1.5.firmware", "servomotor", "SM", ".firmware", false,
     "SM_M17_fw0.14.0.0_scc3_hw1.5");

  bad("servoMotor_caps.firmware", "servomotor", "SM", ".firmware", false);
  bad("servomotor_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.firmware", "servomotor", "SM", ".firmware", false);

  printf("OK\n");
  return 0;
}

