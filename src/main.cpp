#include <Arduino.h>
#include "swd_min.h"

static const swd_min::Pins PINS(35, 36, 37);

static void print_idcode_attempt() {
  uint8_t ack = 0;
  uint32_t idcode = 0;

  swd_min::reset_and_switch_to_swd();

  const bool ok = swd_min::read_idcode(&idcode, &ack);

  Serial.printf("SWD ACK: %u (%s)\n", ack, swd_min::ack_to_str(ack));
  if (ok) {
    Serial.printf("DP IDCODE: 0x%08lX\n", (unsigned long)idcode);
  } else {
    Serial.println("DP IDCODE read failed.");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("\nESP32-S3 SWD minimal: read DP IDCODE");
  Serial.println("Wiring: GPIO35=SWCLK GPIO36=SWDIO GPIO37=NRST");
  Serial.println("Commands:");
  Serial.println("  i = reset + read IDCODE");
  Serial.println("  a = auto: retry read IDCODE 10 times");

  swd_min::begin(PINS);

  // Do one attempt automatically on boot.
  print_idcode_attempt();
}

void loop() {
  if (!Serial.available()) {
    delay(10);
    return;
  }

  const char c = (char)Serial.read();
  if (c == 'i') {
    print_idcode_attempt();
    return;
  }

  if (c == 'a') {
    for (int i = 0; i < 10; i++) {
      Serial.printf("\nAttempt %d/10\n", i + 1);
      print_idcode_attempt();
      delay(200);
    }
    return;
  }
}
