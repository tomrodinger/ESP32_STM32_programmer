#pragma once

#include <Arduino.h>

namespace stm32g0_prog {

// Minimal read-at-offset interface used for filesystem-backed firmware.
//
// Contract:
// - size() returns the underlying file size in bytes
// - read_at(offset, dst, n, &out_n) reads up to n bytes starting at offset
// - if offset == size(), out_n must be 0
// - offsets must be supported in ascending order (random access is preferred but not required)
class FirmwareReader {
 public:
  virtual ~FirmwareReader() = default;
  virtual uint32_t size() const = 0;
  virtual bool read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) = 0;
};

// Target specifics (STM32G031)
static constexpr uint32_t FLASH_BASE = 0x08000000u;
static constexpr uint32_t FLASH_SIZE_BYTES = 0x10000u;     // 64KB
static constexpr uint32_t FLASH_PAGE_SIZE_BYTES = 2048u;   // 2KB

// Connect to target over SWD and halt the core.
bool connect_and_halt();

// Aggressive connect+halt intended for production commands where target firmware may
// disable SWD pins very quickly after reset.
//
// Strategy:
// - While NRST is LOW, power up DP and pre-stage AHB-AP (CSW+TAR=DHCSR)
// - Release NRST and immediately blast a DHCSR halt write in the critical window
// - Re-establish DP after reset, then confirm core is halted
bool connect_and_halt_under_reset_recovery();

// Flash operations
bool flash_mass_erase();
// Mass erase while NRST is held LOW - for recovering chips where firmware disables SWD.
// See MASS_ERASE.md for implementation details.
bool flash_mass_erase_under_reset();
bool flash_program(uint32_t addr, const uint8_t *data, uint32_t len);

// File/stream-backed programming.
// Reads 8 bytes at a time (STM32G0 doubleword programming), padding past EOF with 0xFF.
bool flash_program_reader(uint32_t addr, FirmwareReader &r);

// Verify + dump bytes read from flash. Returns true only if all bytes match.
bool flash_verify_and_dump(uint32_t addr, const uint8_t *data, uint32_t len);

// Fast verify for production use.
// - Uses an AHB-AP session (bulk reads)
// - Avoids per-line dumps (Serial printing dominates runtime)
// - Optionally prints up to max_report mismatches (address + expected/got)
// Returns true only if all bytes match.
bool flash_verify_fast(uint32_t addr, const uint8_t *data, uint32_t len, uint32_t *mismatch_count_out,
                       uint32_t max_report);

// File/stream-backed fast verify.
// Verifies up to round_up(file_size, 8) bytes (matches flash_program() padding behavior).
// Reads 4 bytes at a time (word compare) and pads past EOF with 0xFF.
bool flash_verify_fast_reader(uint32_t addr, FirmwareReader &r, uint32_t *mismatch_count_out, uint32_t max_report);

// Read arbitrary bytes from target memory via SWD/AHB-AP.
// This is used for flash reads (e.g. addr=FLASH_BASE) but is generic.
//
// For the "read first 8 flash bytes" use case described in READ_FLASH.md:
//   flash_read_bytes(0x08000000, out, 8, &flash_optr)
//
// If flash_optr_out is non-null, this routine will attempt a diagnostic read of:
//   FLASH_OPTR at absolute address 0x40022020 (FLASH_R_BASE + 0x20), and store it.
bool flash_read_bytes(uint32_t addr, uint8_t *out, uint32_t len, uint32_t *flash_optr_out = nullptr);

// Read the Program Counter register to verify core is running/accessible.
// Reads PC multiple times to show it's changing (proves core is executing).
// Returns true if successful.
bool read_program_counter();

// Best-effort helper to let the target run normally after we've been debugging.
// Clears vector-catch-on-reset (DEMCR.VC_CORERESET) and clears core halt request
// (DHCSR.C_HALT). Leaves debug enabled (DHCSR.C_DEBUGEN) so we can still re-attach
// without power-cycling.
bool prepare_target_for_normal_run();

} // namespace stm32g0_prog
