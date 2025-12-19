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

// Reset pin control.
void set_nrst(bool asserted);

// --- DP/AP access (sufficient for AHB-AP memory access) ---

// DP register addresses (byte addresses; only bits [3:2] are used on the wire)
static constexpr uint8_t DP_ADDR_IDCODE   = 0x00;
static constexpr uint8_t DP_ADDR_ABORT    = 0x00;
static constexpr uint8_t DP_ADDR_CTRLSTAT = 0x04;
static constexpr uint8_t DP_ADDR_SELECT   = 0x08;
static constexpr uint8_t DP_ADDR_RDBUFF   = 0x0C;

// AP register addresses (byte addresses)
static constexpr uint8_t AP_ADDR_CSW = 0x00;
static constexpr uint8_t AP_ADDR_TAR = 0x04;
static constexpr uint8_t AP_ADDR_DRW = 0x0C;
static constexpr uint8_t AP_ADDR_IDR = 0xFC;

// Establish SWD, power up debug/system, clear sticky errors.
bool dp_init_and_power_up();

bool dp_read_reg(uint8_t addr, uint32_t *val_out, uint8_t *ack_out = nullptr);
bool dp_write_reg(uint8_t addr, uint32_t val, uint8_t *ack_out = nullptr);

// Select AP # and bank. (For STM32G0 typically APSEL=0.)
bool ap_select(uint8_t apsel, uint8_t apbanksel);

// AP read is *posted* in SWD: this helper returns the true value via RDBUFF.
bool ap_read_reg(uint8_t addr, uint32_t *val_out, uint8_t *ack_out = nullptr);
bool ap_write_reg(uint8_t addr, uint32_t val, uint8_t *ack_out = nullptr);

// AHB-AP memory access helpers (32-bit).
bool mem_write32(uint32_t addr, uint32_t val);
bool mem_read32(uint32_t addr, uint32_t *val_out);

// Helper for printing ACK values.
const __FlashStringHelper *ack_to_str(uint8_t ack);

} // namespace swd_min
