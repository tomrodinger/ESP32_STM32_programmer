#include "stm32g0_prog.h"

#include <cstring>

#include "swd_min.h"

namespace stm32g0_prog {

// --- Core/debug/flash registers ---

// CoreDebug + DWT not needed.
// Debug Halting Control and Status Register
static constexpr uint32_t DHCSR = 0xE000EDF0u;
static constexpr uint32_t DHCSR_DBGKEY = 0xA05F0000u;
static constexpr uint32_t DHCSR_C_DEBUGEN = (1u << 0);
static constexpr uint32_t DHCSR_C_HALT = (1u << 1);
static constexpr uint32_t DHCSR_C_DEBUGEN_C_HALT = DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT;
static constexpr uint32_t DHCSR_S_HALT = (1u << 17);

// STM32G0 Flash registers (RM0444)
static constexpr uint32_t FLASH_REG_BASE = 0x40022000u;
static constexpr uint32_t FLASH_KEYR = FLASH_REG_BASE + 0x08u;
static constexpr uint32_t FLASH_SR = FLASH_REG_BASE + 0x10u;
static constexpr uint32_t FLASH_CR = FLASH_REG_BASE + 0x14u;
static constexpr uint32_t FLASH_OPTR = FLASH_REG_BASE + 0x20u;

// Keys
static constexpr uint32_t FLASH_KEY1 = 0x45670123u;
static constexpr uint32_t FLASH_KEY2 = 0xCDEF89ABu;

// FLASH_SR bits
static constexpr uint32_t FLASH_SR_BSY = (1u << 16);

// FLASH_CR bits
static constexpr uint32_t FLASH_CR_PG = (1u << 0);
static constexpr uint32_t FLASH_CR_MER1 = (1u << 2);
// static constexpr uint32_t FLASH_CR_PNB_MASK = (0x7Fu << 3); // (unused: page erase not implemented yet)
static constexpr uint32_t FLASH_CR_STRT = (1u << 16);
static constexpr uint32_t FLASH_CR_LOCK = (1u << 31);

static bool wait_flash_not_busy(uint32_t timeout_ms) {
  const uint32_t start = millis();
  while ((millis() - start) < timeout_ms) {
    uint32_t sr = 0;
    if (!swd_min::mem_read32(FLASH_SR, &sr)) return false;
    if ((sr & FLASH_SR_BSY) == 0) return true;
    delay(1);
  }
  return false;
}

static bool flash_unlock() {
  uint32_t cr = 0;
  if (!swd_min::mem_read32(FLASH_CR, &cr)) return false;
  if ((cr & FLASH_CR_LOCK) == 0) return true;

  Serial.println("FLASH_CR locked; unlocking...");
  if (!swd_min::mem_write32(FLASH_KEYR, FLASH_KEY1)) return false;
  if (!swd_min::mem_write32(FLASH_KEYR, FLASH_KEY2)) return false;

  if (!swd_min::mem_read32(FLASH_CR, &cr)) return false;
  if (cr & FLASH_CR_LOCK) {
    Serial.println("ERROR: Flash unlock failed (LOCK still set)");
    return false;
  }
  return true;
}

static bool flash_clear_cr_bits(uint32_t mask) {
  uint32_t cr = 0;
  if (!swd_min::mem_read32(FLASH_CR, &cr)) return false;
  cr &= ~mask;
  return swd_min::mem_write32(FLASH_CR, cr);
}

bool connect_and_halt() {
  // Reset + switch to SWD, then init/power-up DP.
  swd_min::reset_and_switch_to_swd();

  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed");
    return false;
  }

  // Leave NRST asserted for now; halt should still work since debug is powered.
  // Enable debug + halt core.
  if (!swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_C_HALT)) {
    Serial.println("ERROR: write DHCSR failed");
    return false;
  }

  // Confirm halted.
  for (int i = 0; i < 50; i++) {
    uint32_t dhcsr = 0;
    if (swd_min::mem_read32(DHCSR, &dhcsr) && (dhcsr & DHCSR_S_HALT)) {
      return true;
    }
    delay(1);
  }

  Serial.println("WARN: core did not report HALT; continuing anyway");
  return true;
}

bool flash_read_bytes(uint32_t addr, uint8_t *out, uint32_t len, uint32_t *flash_optr_out) {
  if (len == 0) return true;
  if (!out) return false;

  // Follow READ_FLASH.md checklist:
  // 0) attach + DP init/power-up
  // 1) release NRST
  // 2) short delay
  // 3) halt core
  // 4) perform AHB-AP reads
  swd_min::reset_and_switch_to_swd();
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed");
    return false;
  }

  // Recommended: don't keep target held in reset for memory reads.
  swd_min::set_nrst(false);
  delay(5);

  // Halt core (best-effort). We deliberately avoid calling connect_and_halt() because it would
  // re-run reset_and_switch_to_swd() and re-assert NRST.
  if (!swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_C_HALT)) {
    Serial.println("ERROR: write DHCSR failed");
    return false;
  }
  for (int i = 0; i < 50; i++) {
    uint32_t dhcsr = 0;
    if (swd_min::mem_read32(DHCSR, &dhcsr) && (dhcsr & DHCSR_S_HALT)) break;
    delay(1);
  }

  if (flash_optr_out) {
    uint32_t optr = 0;
    if (swd_min::mem_read32(FLASH_OPTR, &optr)) {
      *flash_optr_out = optr;
    } else {
      // If we can't read it, still continue with the flash read attempt.
      *flash_optr_out = 0;
    }
  }

  // Read bytes via 32-bit transfers.
  // Keep it simple: read 32-bit words and memcpy out; support unaligned/odd lengths.
  uint32_t off = 0;
  while (off < len) {
    const uint32_t aligned_addr = (addr + off) & ~0x3u;
    uint32_t word = 0;
    if (!swd_min::mem_read32(aligned_addr, &word)) {
      Serial.printf("ERROR: mem_read32 failed at 0x%08lX\n", (unsigned long)aligned_addr);
      return false;
    }

    uint8_t bytes[4];
    memcpy(bytes, &word, 4);
    for (uint32_t i = 0; i < 4 && off < len; i++) {
      const uint32_t a = aligned_addr + i;
      if (a < addr) continue;
      out[off++] = bytes[i];
    }
  }

  return true;
}

bool flash_mass_erase() {
  if (!wait_flash_not_busy(/*timeout_ms=*/5000)) {
    Serial.println("ERROR: flash busy timeout before erase");
    return false;
  }
  if (!flash_unlock()) return false;

  Serial.println("Mass erase (MER1)...");

  // Set MER1, then STRT.
  if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_MER1)) return false;
  if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_MER1 | FLASH_CR_STRT)) return false;

  if (!wait_flash_not_busy(/*timeout_ms=*/30000)) {
    Serial.println("ERROR: flash busy timeout during mass erase");
    return false;
  }

  // Clear MER1 + STRT (best practice)
  if (!flash_clear_cr_bits(FLASH_CR_MER1 | FLASH_CR_STRT)) return false;

  Serial.println("Mass erase done");
  return true;
}

bool flash_program(uint32_t addr, const uint8_t *data, uint32_t len) {
  if (!data || len == 0) return true;

  if (!wait_flash_not_busy(/*timeout_ms=*/5000)) {
    Serial.println("ERROR: flash busy timeout before program");
    return false;
  }
  if (!flash_unlock()) return false;

  Serial.printf("Programming %lu bytes at 0x%08lX...\n", (unsigned long)len, (unsigned long)addr);

  // STM32G0 supports 64-bit (double word) programming. We'll write as two 32-bit words.
  // Length must be a multiple of 8 for clean programming; pad with 0xFF if needed.

  uint32_t i = 0;
  while (i < len) {
    // Set PG
    if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_PG)) return false;

    uint8_t chunk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint32_t remaining = len - i;
    const uint32_t to_copy = (remaining >= 8) ? 8 : remaining;
    memcpy(chunk, data + i, to_copy);

    uint32_t w0 = 0, w1 = 0;
    memcpy(&w0, &chunk[0], 4);
    memcpy(&w1, &chunk[4], 4);

    // Write 64-bit payload
    if (!swd_min::mem_write32(addr + i + 0, w0)) return false;
    if (!swd_min::mem_write32(addr + i + 4, w1)) return false;

    if (!wait_flash_not_busy(/*timeout_ms=*/50)) {
      Serial.printf("ERROR: flash busy timeout at offset 0x%lX\n", (unsigned long)i);
      return false;
    }

    // Clear PG
    if (!flash_clear_cr_bits(FLASH_CR_PG)) return false;

    if ((i % 1024u) == 0) Serial.print('.');
    i += 8;
  }

  Serial.println("\nProgram done");
  return true;
}

static void print_hex_line(uint32_t base_addr, const uint8_t *buf, uint32_t n) {
  Serial.printf("0x%08lX: ", (unsigned long)base_addr);
  for (uint32_t i = 0; i < n; i++) {
    Serial.printf("%02X ", buf[i]);
  }
  Serial.println();
}

bool flash_verify_and_dump(uint32_t addr, const uint8_t *data, uint32_t len) {
  if (!data || len == 0) return true;

  Serial.printf("Verify %lu bytes at 0x%08lX (printing bytes read)\n", (unsigned long)len, (unsigned long)addr);

  uint32_t mismatches = 0;

  // Read 32-bit at a time for speed; still dump as bytes.
  for (uint32_t off = 0; off < len; off += 16) {
    uint8_t read_buf[16];
    const uint32_t chunk = (len - off >= 16) ? 16 : (len - off);

    // Fill via 32-bit reads
    for (uint32_t i = 0; i < chunk; i += 4) {
      uint32_t v = 0;
      if (!swd_min::mem_read32(addr + off + i, &v)) {
        Serial.printf("ERROR: mem_read32 failed at 0x%08lX\n", (unsigned long)(addr + off + i));
        return false;
      }
      memcpy(&read_buf[i], &v, (chunk - i >= 4) ? 4 : (chunk - i));
    }

    // Print what we read
    print_hex_line(addr + off, read_buf, chunk);

    // Compare
    for (uint32_t i = 0; i < chunk; i++) {
      const uint8_t exp = data[off + i];
      const uint8_t got = read_buf[i];
      if (got != exp) mismatches++;
    }
  }

  Serial.printf("Verify complete. Bytes differed: %lu\n", (unsigned long)mismatches);
  return mismatches == 0;
}

} // namespace stm32g0_prog
