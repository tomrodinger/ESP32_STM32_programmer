#pragma once

#include <Arduino.h>

// Minimal SWD bit-bang implementation: just enough to read DP IDCODE.
// Wiring per README.md:
//   GPIO35 -> SWCLK
//   GPIO36 -> SWDIO
//   GPIO37 -> nRESET

namespace swd_min {

struct Pins {
  int swclk;
  int swdio;
  int nrst;

  // Keep this C++11-friendly (Arduino/PlatformIO default).
  constexpr Pins() : swclk(35), swdio(36), nrst(37) {}
  constexpr Pins(int swclk_, int swdio_, int nrst_) : swclk(swclk_), swdio(swdio_), nrst(nrst_) {}
};

// SWD ACK values (3-bit field, LSB-first on the wire)
static constexpr uint8_t ACK_OK    = 0b001;
static constexpr uint8_t ACK_WAIT  = 0b010;
static constexpr uint8_t ACK_FAULT = 0b100;

void begin(const Pins &pins);

// Drives a reset + SWD line reset + JTAG->SWD sequence.
void reset_and_switch_to_swd();

// Read DP IDCODE (DP register address 0x00).
// Returns true only if ACK==OK and parity matches.
bool read_idcode(uint32_t *idcode_out, uint8_t *ack_out = nullptr);

// Helper for printing ACK values.
const __FlashStringHelper *ack_to_str(uint8_t ack);

} // namespace swd_min
