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

// Release SWD pins (SWCLK/SWDIO) to high-impedance INPUT.
// This is useful when you want the target firmware to run and potentially repurpose
// those pins as GPIO without electrical contention from the host.
void release_swd_pins();

// Release SWD pins (SWCLK/SWDIO) AND NRST to high-impedance INPUT.
// This is useful when you want the target to reboot/run without the jig driving
// any of the SWD-related pins electrically.
void release_swd_and_nrst_pins();

// Enable verbose SWD diagnostics printed to Serial (best for bench debugging).
// Default: true.
void set_verbose(bool enabled);
bool verbose_enabled();

// Drives a reset + SWD line reset + JTAG->SWD sequence.
void reset_and_switch_to_swd();

// Perform SWD line reset + JTAG-to-SWD WITHOUT touching NRST.
// Use this after releasing NRST to re-establish SWD link on STM32G0,
// because system reset clears the DP/AP state.
void swd_line_reset();

// "Connect under reset" sequence: release NRST and aggressively try to
// re-establish SWD communication before user firmware can disable SWD pins.
// Call this AFTER reset_and_switch_to_swd() and dp_init_and_power_up() succeed
// with NRST held low.
bool connect_under_reset_and_init();

// Convenience helper used for the bench-proven “attach + IDCODE read” sequence.
// This is the only place we print the attach banner lines.
bool attach_and_read_idcode(uint32_t *idcode_out, uint8_t *ack_out = nullptr);

// Read DP IDCODE (DP register address 0x00).
// Returns true only if ACK==OK and parity matches.
bool read_idcode(uint32_t *idcode_out, uint8_t *ack_out = nullptr);

// Reset pin control.
void set_nrst(bool asserted);

// Reset pin control without emitting the "NRST HIGH/LOW" banner line.
// This is used in the critical timing window where the target firmware may
// reconfigure SWD pins immediately after reset release.
void set_nrst_quiet(bool asserted);

// Returns the current output level on the NRST pin (true = HIGH, false = LOW).
// NOTE: NRST is driven by the ESP32, so this reflects what we are driving.
bool nrst_is_high();

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

// AP write variant optimized for bulk transfers:
// - no post-transfer idle/flush clocks
// - no human logging
// Intended for performance-critical loops (e.g. flash programming).
bool ap_write_reg_fast(uint8_t addr, uint32_t val, uint8_t *ack_out = nullptr);

// Critical-window AP write: performs a single AP write with minimal post-transaction
// overhead (no post-idle cycles, no human logging). Intended for the first DHCSR halt
// write right after NRST release.
bool ap_write_reg_critical(uint8_t addr, uint32_t val, uint8_t *ack_out = nullptr);

// Lightweight AHB-AP session that avoids re-writing SELECT/CSW/TAR on every 32-bit access.
// This is a major performance win for flash programming where accesses are sequential.
struct AhbApSession {
  bool begin();
  void invalidate();

  bool write32(uint32_t addr, uint32_t val);
  bool read32(uint32_t addr, uint32_t *val_out);

  // Bulk sequential 32-bit reads optimized using AP posted-read pipelining.
  // Reads `words` consecutive 32-bit words starting at `addr` into `out_words`.
  // Returns false on any SWD transaction failure.
  bool read32_pipelined(uint32_t addr, uint32_t *out_words, uint32_t words);

 private:
  bool tar_valid_ = false;
  uint32_t tar_ = 0;
};

// AHB-AP memory access helpers (32-bit).
bool mem_write32(uint32_t addr, uint32_t val);
bool mem_read32(uint32_t addr, uint32_t *val_out);

// Human-friendly variants: print one condensed English line per DP/AP read/write
// (purpose + register + address + data + ACK status).
bool mem_write32_verbose(const char *purpose, uint32_t addr, uint32_t val);
bool mem_read32_verbose(const char *purpose, uint32_t addr, uint32_t *val_out);

// Helper for printing ACK values.
// NOTE: We return a plain C string so it is safe to use with Serial.printf("%s").
const char *ack_to_str(uint8_t ack);

} // namespace swd_min
