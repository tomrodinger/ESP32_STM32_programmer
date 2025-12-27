#include "mode2_loop.h"

#include <Arduino.h>

#include "tee_log.h"

static inline Print &LOG() { return tee_log::out(); }

static void print_user_pressed_banner(char c) {
  if (c >= 32 && c <= 126) {
    LOG().printf("=== [Mode 2] User pressed '%c' (0x%02X) ===\n", c, (unsigned char)c);
  } else {
    LOG().printf("=== [Mode 2] User pressed 0x%02X ===\n", (unsigned char)c);
  }
}

static void print_help() {
  LOG().println("Mode 2: RS485 Testing Mode");
  LOG().println("Commands:");
  LOG().println("  h = help");
  LOG().println("  1 = switch to Mode 1 (SWD Programming)");
  LOG().println("  2 = stay in Mode 2 (this mode)");
  LOG().println("  t = test (prints 'Testing... test done!')");
}

static void print_mode2_banner() {
  LOG().println("========================================");
  LOG().println("          MODE 2: RS485 Testing        ");
  LOG().println("========================================");
  LOG().println("Press 'h' for help, '1' to switch to SWD mode");
}

namespace mode2_loop {

void run() {
  print_mode2_banner();

  while (true) {
    if (!Serial.available()) {
      delay(10);
      continue;
    }

    const char c = (char)Serial.read();

    // Ignore whitespace/newlines.
    if (c == '\n' || c == '\r' || c == ' ') {
      continue;
    }

    print_user_pressed_banner(c);

    switch (c) {
      case 'h':
      case '?':
        print_help();
        break;

      case '1':
        LOG().println("Switching to Mode 1 (SWD Programming)...");
        return;

      case '2':
        LOG().println("Already in Mode 2 (RS485 Testing)");
        break;

      case 't':
        LOG().println("Testing... test done!");
        break;

      default:
        LOG().printf("Unknown command '%c'. Press 'h' for help.\n", c);
        break;
    }
  }
}

}  // namespace mode2_loop

