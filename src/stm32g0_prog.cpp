#include "stm32g0_prog.h"

#include <cstring>

#include "swd_min.h"

namespace stm32g0_prog {

static inline bool verbose() { return swd_min::verbose_enabled(); }

// --- Core/debug/flash registers ---

// Debug Halting Control and Status Register
static constexpr uint32_t DHCSR = 0xE000EDF0u;
static constexpr uint32_t DHCSR_DBGKEY = 0xA05F0000u;
static constexpr uint32_t DHCSR_C_DEBUGEN = (1u << 0);
static constexpr uint32_t DHCSR_C_HALT = (1u << 1);
static constexpr uint32_t DHCSR_C_DEBUGEN_C_HALT = DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT;
static constexpr uint32_t DHCSR_S_HALT = (1u << 17);
static constexpr uint32_t DHCSR_S_REGRDY = (1u << 16);     // Register transfer ready
static constexpr uint32_t DHCSR_S_RESET_ST = (1u << 25);   // Reset sticky bit

// Debug Core Register Selector/Data Registers (ARM CoreSight)
// Used to read/write CPU core registers (R0-R15, PSR, etc.)
// Source: ARM Cortex-M0+ Technical Reference Manual
static constexpr uint32_t DCRSR = 0xE000EDF4u;  // Debug Core Register Selector Register
static constexpr uint32_t DCRDR = 0xE000EDF8u;  // Debug Core Register Data Register
static constexpr uint32_t DCRSR_REGWNR = (1u << 16);  // 0=read, 1=write
// Core register numbers for DCRSR[4:0]
static constexpr uint32_t REGNUM_PC = 15u;      // Program Counter (R15)

// Debug Exception and Monitor Control Register - used for vector catch on reset
static constexpr uint32_t DEMCR = 0xE000EDFCu;
static constexpr uint32_t DEMCR_VC_CORERESET = (1u << 0);  // Vector catch: halt on reset
static constexpr uint32_t DEMCR_TRCENA = (1u << 24);       // Enable DWT/ITM

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

  // FLASH_SR completion + error flags (STM32G031)
  // Bit positions confirmed in [`FLASH_ERASE.md`](FLASH_ERASE.md:88) via [`docs/stm32g031xx.h`](docs/stm32g031xx.h:2440)
  static constexpr uint32_t FLASH_SR_EOP = (1u << 0);
  static constexpr uint32_t FLASH_SR_OPERR = (1u << 1);
  static constexpr uint32_t FLASH_SR_PROGERR = (1u << 3);
  static constexpr uint32_t FLASH_SR_WRPERR = (1u << 4);
  static constexpr uint32_t FLASH_SR_PGAERR = (1u << 5);
  static constexpr uint32_t FLASH_SR_SIZERR = (1u << 6);
  static constexpr uint32_t FLASH_SR_PGSERR = (1u << 7);
  static constexpr uint32_t FLASH_SR_MISERR = (1u << 8);
  static constexpr uint32_t FLASH_SR_FASTERR = (1u << 9);
  static constexpr uint32_t FLASH_SR_RDERR = (1u << 14);
  static constexpr uint32_t FLASH_SR_OPTVERR = (1u << 15);
  static constexpr uint32_t FLASH_SR_ALL_ERRORS =
      FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR |
      FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR | FLASH_SR_OPTVERR;
  static constexpr uint32_t FLASH_SR_CLEAR_MASK = FLASH_SR_EOP | FLASH_SR_ALL_ERRORS;

  // FLASH_CR bits
  static constexpr uint32_t FLASH_CR_PG = (1u << 0);
  static constexpr uint32_t FLASH_CR_PER = (1u << 1);
  static constexpr uint32_t FLASH_CR_MER1 = (1u << 2);
  // static constexpr uint32_t FLASH_CR_PNB_MASK = (0x7Fu << 3); // (unused: page erase not implemented yet)
  static constexpr uint32_t FLASH_CR_STRT = (1u << 16);
  static constexpr uint32_t FLASH_CR_LOCK = (1u << 31);

static bool wait_flash_not_busy(uint32_t timeout_ms, swd_min::AhbApSession *s = nullptr) {
  // IMPORTANT: A 1ms delay inside this polling loop makes programming extremely slow
  // because per-doubleword flash programming busy time is typically far below 1ms.
  // Use microsecond-scale backoff for short operations.
  const uint32_t start_us = micros();
  const uint32_t timeout_us = timeout_ms * 1000u;

  while ((uint32_t)(micros() - start_us) < timeout_us) {
    uint32_t sr = 0;
    const bool ok = s ? s->read32(FLASH_SR, &sr) : swd_min::mem_read32(FLASH_SR, &sr);
    if (!ok) return false;
    if ((sr & FLASH_SR_BSY) == 0) return true;

    // Backoff tuned by expected operation duration.
    if (timeout_ms >= 1000u) {
      delay(1);
    } else {
      delayMicroseconds(50);
    }
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

static bool flash_unlock_fast(swd_min::AhbApSession &ap) {
  uint32_t cr = 0;
  if (!ap.read32(FLASH_CR, &cr)) return false;
  if ((cr & FLASH_CR_LOCK) == 0) return true;

  Serial.println("FLASH_CR locked; unlocking...");
  if (!ap.write32(FLASH_KEYR, FLASH_KEY1)) return false;
  if (!ap.write32(FLASH_KEYR, FLASH_KEY2)) return false;

  if (!ap.read32(FLASH_CR, &cr)) return false;
  if (cr & FLASH_CR_LOCK) {
    Serial.println("ERROR: Flash unlock failed (LOCK still set)");
    return false;
  }
  return true;
}

static bool flash_clear_sr_flags(uint32_t mask) {
  // STM32G0: FLASH_SR flags are W1C (write 1 to clear) per ST HAL
  // See: [`FLASH_ERASE.md`](FLASH_ERASE.md:110) and [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:786)
  if ((mask & FLASH_SR_CLEAR_MASK) == 0) return true;
  return swd_min::mem_write32(FLASH_SR, mask & FLASH_SR_CLEAR_MASK);
}

static bool flash_clear_sr_flags_fast(swd_min::AhbApSession &ap, uint32_t mask) {
  if ((mask & FLASH_SR_CLEAR_MASK) == 0) return true;
  return ap.write32(FLASH_SR, mask & FLASH_SR_CLEAR_MASK);
}

static bool flash_clear_cr_bits(uint32_t mask) {
  uint32_t cr = 0;
  if (!swd_min::mem_read32(FLASH_CR, &cr)) return false;
  cr &= ~mask;
  return swd_min::mem_write32(FLASH_CR, cr);
}

bool connect_and_halt() {
  if (verbose()) {
    Serial.println("Step 1/4: Assert reset and switch the debug port to SWD mode...");
  }
  // Reset + switch to SWD, then init/power-up DP.
  swd_min::reset_and_switch_to_swd();

  if (verbose()) {
    Serial.println("Step 2/4: Power up the debug and system domains (DP power-up handshake)...");
  }
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed (pre-NRST release)");
    return false;
  }

  // For flash operations, do not keep the target held in reset.
  // See safety note in [`FLASH_ERASE.md`](FLASH_ERASE.md:139).
  //
  // IMPORTANT: On STM32G0, releasing NRST performs a system reset that clears
  // the DP/AP state. User firmware may also disable SWD pins extremely quickly
  // (in microseconds). Use the aggressive "connect under reset" sequence.
  // See: Perplexity research on STM32G0 reset behavior and "connect under reset".
  if (verbose()) {
    Serial.println("Step 3/4: Release reset and immediately re-connect over SWD (connect-under-reset)...");
  }
  if (!swd_min::connect_under_reset_and_init()) {
    Serial.println("ERROR: connect_under_reset_and_init failed");
    return false;
  }

  if (verbose()) {
    Serial.println("Step 4/4: Enable debugging and halt the CPU core so memory reads are stable...");
  }
  // Enable debug + halt core.
  if (verbose()) {
    Serial.println("Writing DHCSR to enable debugging and halt the CPU...");
  }
  if (!swd_min::mem_write32_verbose("Enable debug and halt the CPU (DHCSR)", DHCSR, DHCSR_C_DEBUGEN_C_HALT)) {
    Serial.println("ERROR: write DHCSR failed");
    return false;
  }

  // Confirm halted.
  for (int i = 0; i < 50; i++) {
    uint32_t dhcsr = 0;
    if (swd_min::mem_read32_verbose("Read DHCSR status to confirm the CPU is halted", DHCSR, &dhcsr) &&
        (dhcsr & DHCSR_S_HALT)) {
      return true;
    }
    delay(1);
  }

  Serial.println("WARN: core did not report HALT; continuing anyway");
  return true;
}

bool connect_and_halt_under_reset_recovery() {
  // Production-oriented connect+halt flow.
  //
  // Rationale:
  // Some target firmwares re-purpose SWD pins extremely quickly after reset.
  // A "normal" connect flow can spend too long re-syncing/printing after NRST release.
  //
  // Strategy (same critical-window technique as [`flash_mass_erase_under_reset()`](src/stm32g0_prog.cpp:312)):
  //  1) While NRST LOW: reset+SWD, DP init+power-up, pre-stage AHB-AP (CSW+TAR=DHCSR)
  //  2) Release NRST and immediately write DHCSR halt in the critical window
  //  3) Re-init DP and confirm core is halted

  if (verbose()) {
    Serial.println("Connect recovery: connect-under-reset + immediate halt...");
  }

  // Step 1: Assert reset and switch the debug port to SWD mode.
  swd_min::reset_and_switch_to_swd();

  // Step 2: DP init + power-up (NRST LOW).
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed (NRST LOW)");
    return false;
  }

  // Step 3: Best-effort arm halt-on-reset while NRST LOW.
  (void)swd_min::mem_write32(DEMCR, DEMCR_VC_CORERESET);
  (void)swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_C_HALT);

  // Step 3b: Pre-stage AHB-AP so the next DRW write targets DHCSR.
  if (!swd_min::ap_select(/*apsel=*/0, /*apbanksel=*/0)) {
    Serial.println("ERROR: ap_select failed");
    return false;
  }

  // Matches the AHB-AP CSW used by [`swd_min::mem_write32()`](src/swd_min.cpp:929).
  static constexpr uint32_t CSW_32_INC = 0x23000012u;
  uint8_t ack = 0;
  if (!swd_min::ap_write_reg(swd_min::AP_ADDR_CSW, CSW_32_INC, &ack) || ack != swd_min::ACK_OK) {
    Serial.printf("ERROR: AP CSW write failed, ACK=%u (%s)\n", (unsigned)ack, swd_min::ack_to_str(ack));
    return false;
  }
  if (!swd_min::ap_write_reg(swd_min::AP_ADDR_TAR, DHCSR, &ack) || ack != swd_min::ACK_OK) {
    Serial.printf("ERROR: AP TAR write failed, ACK=%u (%s)\n", (unsigned)ack, swd_min::ack_to_str(ack));
    return false;
  }

  // Step 4: Critical window: release NRST and immediately write DHCSR halt.
  swd_min::set_nrst_quiet(false);
  uint8_t first_halt_ack = 0;
  (void)swd_min::ap_write_reg_critical(swd_min::AP_ADDR_DRW, DHCSR_C_DEBUGEN_C_HALT, &first_halt_ack);

  ack = first_halt_ack;
  if (ack != swd_min::ACK_OK) {
    for (int i = 0; i < 8; i++) {
      (void)swd_min::ap_write_reg_critical(swd_min::AP_ADDR_DRW, DHCSR_C_DEBUGEN_C_HALT, &ack);
      if (ack == swd_min::ACK_OK) break;
    }
  }

  if (verbose()) {
    Serial.printf("Immediate halt write ACK=%u (%s)\n", (unsigned)first_halt_ack, swd_min::ack_to_str(first_halt_ack));
  }

  delay(2);

  // Step 5: DP init (NRST HIGH) - try without line reset first.
  if (!swd_min::dp_init_and_power_up()) {
    swd_min::swd_line_reset();
    if (!swd_min::dp_init_and_power_up()) {
      Serial.println("ERROR: dp_init_and_power_up failed (NRST HIGH)");
      return false;
    }
  }

  // Step 6: Force debug+halt and confirm S_HALT.
  if (!swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_C_HALT)) {
    Serial.println("ERROR: write DHCSR failed");
    return false;
  }
  uint32_t dhcsr = 0;
  if (!swd_min::mem_read32(DHCSR, &dhcsr)) {
    Serial.println("ERROR: read DHCSR failed");
    return false;
  }

  if ((dhcsr & DHCSR_S_HALT) == 0) {
    Serial.println("WARN: core did not report HALT; continuing anyway");
  }
  return true;
}

bool flash_read_bytes(uint32_t addr, uint8_t *out, uint32_t len, uint32_t *flash_optr_out) {
  if (len == 0) return true;
  if (!out) return false;

  // Preconditions:
  // - SWD is already attached and DP is powered up
  // - NRST is released (HIGH)
  // - Core is halted (recommended) so memory reads are stable
  // The 'r' command achieves this by calling connect_and_halt() first.

  if (flash_optr_out) {
    uint32_t optr = 0;
    if (swd_min::mem_read32_verbose("Read FLASH_OPTR (diagnostic: includes RDP byte)", FLASH_OPTR, &optr)) {
      *flash_optr_out = optr;
    } else {
      // If we can't read it, still continue with the flash read attempt.
      *flash_optr_out = 0;
    }
  }

  if (verbose()) {
    Serial.printf("Reading %lu bytes starting at 0x%08lX via AHB-AP...\n", (unsigned long)len, (unsigned long)addr);
  }

  // Read bytes via 32-bit transfers.
  // Keep it simple: read 32-bit words and memcpy out; support unaligned/odd lengths.
  uint32_t off = 0;
  while (off < len) {
    const uint32_t aligned_addr = (addr + off) & ~0x3u;
    uint32_t word = 0;
    if (!swd_min::mem_read32_verbose("Read 32-bit word from target memory", aligned_addr, &word)) {
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
  // Implements the checklist in [`FLASH_ERASE.md`](FLASH_ERASE.md:131).
  if (!wait_flash_not_busy(/*timeout_ms=*/5000)) {
    Serial.println("ERROR: flash busy timeout before erase");
    return false;
  }

  // Clear completion + error flags before starting.
  if (!flash_clear_sr_flags(FLASH_SR_CLEAR_MASK)) {
    Serial.println("ERROR: failed to clear FLASH_SR flags");
    return false;
  }

  if (!flash_unlock()) return false;

  // Clear potentially-conflicting control bits.
  if (!flash_clear_cr_bits(FLASH_CR_PG | FLASH_CR_PER)) return false;

  Serial.println("Mass erase (MER1)...");

  // Set MER1, then STRT.
  if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_MER1)) return false;
  if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_MER1 | FLASH_CR_STRT)) return false;

  if (!wait_flash_not_busy(/*timeout_ms=*/30000)) {
    Serial.println("ERROR: flash busy timeout during mass erase");
    return false;
  }

  // Check outcome.
  uint32_t sr = 0;
  if (!swd_min::mem_read32(FLASH_SR, &sr)) return false;
  if (sr & FLASH_SR_ALL_ERRORS) {
    Serial.printf("ERROR: flash erase error flags set: FLASH_SR=0x%08lX\n", (unsigned long)sr);
    (void)flash_clear_sr_flags(sr);
    return false;
  }

  // Some bench runs have observed FLASH_SR reading as 0x00000000 at completion even
  // when flash contents are actually erased (verified by reading flash back).
  // Treat missing EOP as a warning, not a hard failure; caller may verify flash.
  if ((sr & FLASH_SR_EOP) == 0) {
    Serial.printf("WARN: flash erase did not set EOP: FLASH_SR=0x%08lX\n", (unsigned long)sr);
  }

  // Clear EOP (and any errors if they appeared between reads).
  if (!flash_clear_sr_flags(FLASH_SR_CLEAR_MASK)) return false;

  // Clear MER1 + STRT and lock.
  if (!flash_clear_cr_bits(FLASH_CR_MER1 | FLASH_CR_STRT)) return false;
  if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_LOCK)) return false;

  Serial.println("Mass erase done");
  return true;
}

bool flash_mass_erase_under_reset() {
  // Mass erase using a "connect-under-reset" recovery flow.
  //
  // Goal: recover chips where user firmware disables SWD very quickly after reset.
  // Strategy:
  //  1) While NRST is held LOW, fully initialize DP and pre-stage the AHB-AP (CSW+TAR)
  //     so the very next AP.DRW write targets DHCSR.
  //  2) Release NRST and immediately blast a small burst of DHCSR halt writes.
  //  3) Re-sync SWD and then run the normal mass-erase routine (which assumes NRST HIGH).
  //
  // NOTE: All FLASH register addresses/bit masks used by the underlying erase routine are
  // confirmed in [`FLASH_ERASE.md`](FLASH_ERASE.md:30) via [`docs/stm32g031xx.h`](docs/stm32g031xx.h:2440).

  Serial.println("Mass erase recovery: connect-under-reset + immediate halt, then normal erase...");

  // Step 1: Assert reset and switch the debug port to SWD mode.
  Serial.println("Step 1: Assert NRST LOW and enter SWD mode...");
  swd_min::reset_and_switch_to_swd();

  // Step 2: Initialize DP and power up debug/system domains (NRST still LOW).
  Serial.println("Step 2: DP init + power-up (NRST LOW)...");
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed (NRST LOW)");
    return false;
  }

  // Step 3: While NRST is still LOW, set up the debug core state so that the CPU
  // halts immediately when reset is released.
  //
  // Why do this *before* releasing reset?
  // - When NRST is LOW, the CPU is not executing user firmware, so SWD pins cannot
  //   be reconfigured out from under us.
  // - System debug space (DHCSR/DEMCR) is intended to be reachable even while the
  //   core is held in reset (see repo doc: [`MASS_ERASE.md`](MASS_ERASE.md:9)).
  //
  // This reduces dependence on a microsecond-scale timing window after NRST release.
  Serial.println("Step 3: Arm halt-on-reset while NRST LOW (DEMCR + DHCSR)...");

  // Best-effort: arm vector-catch on core reset.
  // NOTE: Whether DEMCR.VC_CORERESET persists across reset release is implementation-specific.
  (void)swd_min::mem_write32(DEMCR, DEMCR_VC_CORERESET);

  // Best-effort: enable debug and request halt.
  // If this write is latched by the core debug block while the core is in reset, the
  // core should come up halted immediately at reset release.
  (void)swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_C_HALT);

  // Step 3b: Pre-stage AP (SELECT + CSW + TAR) so the next DRW write hits DHCSR.
  Serial.println("Step 3b: Pre-stage AHB-AP (CSW + TAR=DHCSR) while NRST LOW...");
  if (!swd_min::ap_select(/*apsel=*/0, /*apbanksel=*/0)) {
    Serial.println("ERROR: ap_select failed");
    return false;
  }

  static constexpr uint32_t CSW_32_INC = 0x23000012u;  // matches [`swd_min::mem_write32()`](src/swd_min.cpp:788)
  uint8_t ack = 0;
  if (!swd_min::ap_write_reg(swd_min::AP_ADDR_CSW, CSW_32_INC, &ack) || ack != swd_min::ACK_OK) {
    Serial.printf("ERROR: AP CSW write failed, ACK=%u (%s)\n", (unsigned)ack, swd_min::ack_to_str(ack));
    return false;
  }
  if (!swd_min::ap_write_reg(swd_min::AP_ADDR_TAR, DHCSR, &ack) || ack != swd_min::ACK_OK) {
    Serial.printf("ERROR: AP TAR write failed, ACK=%u (%s)\n", (unsigned)ack, swd_min::ack_to_str(ack));
    return false;
  }

  // Step 4: CRITICAL TIMING window.
  // Requirement: after releasing NRST we must NOT do any SWD re-sync/DP init before
  // attempting the first halt write, because user firmware may remap SWD pins in µs.
  //
  // Therefore:
  //  - release NRST without prints (quiet)
  //  - perform exactly one immediate AP.DRW write (pre-staged to DHCSR)
  //  - optionally retry a few times with the tightest possible loop
  Serial.println("Step 4: Release NRST and immediately send FIRST halt write...");
  swd_min::set_nrst_quiet(false);

  // Single immediate write (the critical one).
  uint8_t first_halt_ack = 0;
  (void)swd_min::ap_write_reg_critical(swd_min::AP_ADDR_DRW, DHCSR_C_DEBUGEN_C_HALT, &first_halt_ack);

  // Optional tight retries (no prints, no post-idle cycles) to increase odds.
  // Keep count small; any extra SWCLK edges are still time.
  ack = first_halt_ack;
  if (ack != swd_min::ACK_OK) {
    for (int i = 0; i < 8; i++) {
      (void)swd_min::ap_write_reg_critical(swd_min::AP_ADDR_DRW, DHCSR_C_DEBUGEN_C_HALT, &ack);
      if (ack == swd_min::ACK_OK) break;
    }
  }

  // Exit the critical window: now it's safe to print/delay.
  Serial.println("---------------------------------------- NRST HIGH");
  Serial.printf("Immediate halt write ACK=%u (%s)\n", (unsigned)first_halt_ack, swd_min::ack_to_str(first_halt_ack));

  // Give the core/debug fabric a moment to settle.
  delay(2);

  // Step 5: Re-establish SWD/DP after NRST release.
  // Prefer the *cheapest* thing first: try DP init without doing a full line-reset.
  // If this works, we avoid spending extra time clocking SWD (which matters if the
  // core did not actually halt).
  Serial.println("Step 5: DP init (NRST HIGH) - try without line-reset...");
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("Step 5b: DP init failed; re-sync SWD physical layer then retry DP init...");
    swd_min::swd_line_reset();
    if (!swd_min::dp_init_and_power_up()) {
      Serial.println("ERROR: dp_init_and_power_up failed (NRST HIGH)");
      return false;
    }
  }

  // Step 6: Ensure debug+halt is asserted (safe even if already halted).
  Serial.println("Step 6: Confirm halt (write DHCSR, then read back)...");
  if (!swd_min::mem_write32_verbose("Force debug enable + halt (DHCSR)", DHCSR, DHCSR_C_DEBUGEN_C_HALT)) {
    Serial.println("ERROR: write DHCSR failed");
    return false;
  }
  uint32_t dhcsr = 0;
  if (!swd_min::mem_read32_verbose("Read DHCSR (confirm S_HALT)", DHCSR, &dhcsr)) {
    Serial.println("ERROR: read DHCSR failed");
    return false;
  }
  Serial.printf("DHCSR = 0x%08lX (S_HALT=%u)\n", (unsigned long)dhcsr, (unsigned)((dhcsr & DHCSR_S_HALT) ? 1u : 0u));

  // Step 7: Run the standard mass erase routine (NRST HIGH).
  Serial.println("Step 7: Run normal mass erase... ");
  // NOTE: `flash_mass_erase()` treats missing EOP as a warning (bench-observed).
  // Final success is determined by verifying flash contents below.
  (void)flash_mass_erase();

  // Step 8: Spot-check the first word of flash.
  Serial.println("Step 8: Verify erase (read flash @ 0x08000000)...");
  uint32_t verify_word = 0;
  if (!swd_min::mem_read32(FLASH_BASE, &verify_word)) {
    Serial.println("WARN: Could not read flash for verification");
    return true;
  }
  Serial.printf("Flash[0x%08lX] = 0x%08lX (expect 0xFFFFFFFF)\n", (unsigned long)FLASH_BASE,
                (unsigned long)verify_word);
  if (verify_word != 0xFFFFFFFFu) {
    Serial.println("ERROR: Flash not erased (first word != 0xFFFFFFFF)");
    return false;
  }
  return true;
}

bool flash_program(uint32_t addr, const uint8_t *data, uint32_t len) {
  if (!data || len == 0) return true;

  swd_min::AhbApSession ap;
  if (!ap.begin()) {
    Serial.println("ERROR: AHB-AP session init failed");
    return false;
  }

  if (!wait_flash_not_busy(/*timeout_ms=*/5000, &ap)) {
    Serial.println("ERROR: flash busy timeout before program");
    return false;
  }
  if (!flash_unlock_fast(ap)) return false;

  // Clear completion + error flags once before starting the program operation.
  if (!flash_clear_sr_flags_fast(ap, FLASH_SR_CLEAR_MASK)) {
    Serial.println("ERROR: failed to clear FLASH_SR flags before program");
    return false;
  }

  Serial.printf("Programming %lu bytes at 0x%08lX...\n", (unsigned long)len, (unsigned long)addr);

  // STM32G0 supports 64-bit (double word) programming. We'll write as two 32-bit words.
  // Length must be a multiple of 8 for clean programming; pad with 0xFF if needed.

  // Keep PG set for the whole programming loop.
  // This avoids two FLASH_CR accesses per doubleword (set/clear) which is very costly over bit-banged SWD.
  {
    uint32_t cr = 0;
    if (!ap.read32(FLASH_CR, &cr)) return false;
    cr &= ~(FLASH_CR_PER | FLASH_CR_MER1);
    cr |= FLASH_CR_PG;
    if (!ap.write32(FLASH_CR, cr)) return false;
  }

  uint32_t i = 0;
  while (i < len) {

    uint8_t chunk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint32_t remaining = len - i;
    const uint32_t to_copy = (remaining >= 8) ? 8 : remaining;
    memcpy(chunk, data + i, to_copy);

    uint32_t w0 = 0, w1 = 0;
    memcpy(&w0, &chunk[0], 4);
    memcpy(&w1, &chunk[4], 4);

    // Write 64-bit payload
    if (!ap.write32(addr + i + 0, w0)) return false;
    if (!ap.write32(addr + i + 4, w1)) return false;

    if (!wait_flash_not_busy(/*timeout_ms=*/10, &ap)) {
      Serial.printf("ERROR: flash busy timeout at offset 0x%lX\n", (unsigned long)i);
      return false;
    }

    if ((i % 1024u) == 0) Serial.print('.');
    i += 8;
  }

  // Clear PG and lock flash.
  {
    uint32_t cr = 0;
    if (!ap.read32(FLASH_CR, &cr)) return false;
    cr &= ~FLASH_CR_PG;
    cr |= FLASH_CR_LOCK;
    if (!ap.write32(FLASH_CR, cr)) return false;
  }

  // Clear completion + error flags after programming.
  (void)flash_clear_sr_flags_fast(ap, FLASH_SR_CLEAR_MASK);

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

bool flash_verify_fast(uint32_t addr, const uint8_t *data, uint32_t len, uint32_t *mismatch_count_out,
                       uint32_t max_report) {
  if (mismatch_count_out) *mismatch_count_out = 0;
  if (!data || len == 0) return true;

  // Word-compare only (production speed). Require 32-bit alignment.
  if ((addr & 0x3u) != 0 || (len & 0x3u) != 0) {
    Serial.println("ERROR: fast verify requires 32-bit aligned addr and length (word-compare only)");
    return false;
  }

  swd_min::AhbApSession ap;
  if (!ap.begin()) {
    Serial.println("ERROR: AHB-AP session init failed");
    return false;
  }

  uint32_t mismatches = 0;
  uint32_t reported = 0;

  // Prefer pipelined reads for speed, but validate per-chunk and fall back to safe
  // reads if the pipeline produces inconsistent results on real hardware.
  bool use_pipeline = true;

  const uint32_t total_words = len / 4u;

  if (use_pipeline) {
    static constexpr uint32_t k_words_per_chunk = 64;  // 256 bytes
    uint32_t buf[k_words_per_chunk];

    uint32_t word_index = 0;
    while (word_index < total_words) {
      const uint32_t remaining_words = total_words - word_index;
      const uint32_t chunk_words = (remaining_words > k_words_per_chunk) ? k_words_per_chunk : remaining_words;

      const uint32_t chunk_addr = addr + (word_index * 4u);

      if (!ap.read32_pipelined(chunk_addr, buf, chunk_words)) {
        Serial.printf("WARN: pipelined verify failed at 0x%08lX; retrying with safe reads\n",
                      (unsigned long)chunk_addr);
        use_pipeline = false;
        ap.invalidate();
        break;
      }

      // Validate pipeline on this chunk with a couple of known-correct reads.
      // This catches cases where pipelining seems to work initially but later drifts or
      // returns stale data on real hardware.
      {
        const uint32_t last_addr = chunk_addr + ((chunk_words - 1u) * 4u);

        uint32_t check0 = 0;
        uint32_t check_last = 0;

        if (!ap.read32(chunk_addr, &check0)) {
          Serial.printf("ERROR: verify validation read failed at 0x%08lX\n", (unsigned long)chunk_addr);
          if (mismatch_count_out) *mismatch_count_out = mismatches;
          return false;
        }
        if (!ap.read32(last_addr, &check_last)) {
          Serial.printf("ERROR: verify validation read failed at 0x%08lX\n", (unsigned long)last_addr);
          if (mismatch_count_out) *mismatch_count_out = mismatches;
          return false;
        }

        // Invalidate TAR state before continuing the pipelined loop.
        ap.invalidate();

        if (check0 != buf[0] || check_last != buf[chunk_words - 1u]) {
          Serial.printf("WARN: pipelined AP reads appear unreliable in this region (0x%08lX..0x%08lX); using safe reads\n",
                        (unsigned long)chunk_addr, (unsigned long)last_addr);
          use_pipeline = false;

          // Re-read this chunk using the safe method so we don't produce false mismatches.
          for (uint32_t i = 0; i < chunk_words; i++) {
            if (!ap.read32(chunk_addr + i * 4u, &buf[i])) {
              Serial.printf("ERROR: flash verify read failed at 0x%08lX\n", (unsigned long)(chunk_addr + i * 4u));
              if (mismatch_count_out) *mismatch_count_out = mismatches;
              return false;
            }
          }
          ap.invalidate();
        }
      }

      for (uint32_t i = 0; i < chunk_words; i++) {
        uint32_t exp_word = 0;
        memcpy(&exp_word, data + ((word_index + i) * 4u), 4);
        const uint32_t got_word = buf[i];
        if (got_word != exp_word) {
          mismatches++;
          if (reported < max_report) {
            const uint32_t a = addr + ((word_index + i) * 4u);
            // Extra diagnostic: re-read this word using the known-correct (DP.RDBUFF) path.
            uint32_t got_safe = 0;
            const bool safe_ok = swd_min::mem_read32(a, &got_safe);
            // `mem_read32()` reconfigures AP/DP state, so invalidate the session TAR cache.
            ap.invalidate();

            if (safe_ok) {
              Serial.printf("Mismatch @ 0x%08lX: exp=%08lX got=%08lX (safe=%08lX)\n", (unsigned long)a,
                            (unsigned long)exp_word, (unsigned long)got_word, (unsigned long)got_safe);
            } else {
              Serial.printf("Mismatch @ 0x%08lX: exp=%08lX got=%08lX (safe read FAILED)\n", (unsigned long)a,
                            (unsigned long)exp_word, (unsigned long)got_word);
            }
            reported++;
          }
        }
      }

      word_index += chunk_words;
    }
  }

  if (!use_pipeline) {
    for (uint32_t i = 0; i < total_words; i++) {
      uint32_t got_word = 0;
      if (!ap.read32(addr + i * 4u, &got_word)) {
        Serial.printf("ERROR: flash verify read failed at 0x%08lX\n", (unsigned long)(addr + i * 4u));
        if (mismatch_count_out) *mismatch_count_out = mismatches;
        return false;
      }

      uint32_t exp_word = 0;
      memcpy(&exp_word, data + i * 4u, 4);
      if (got_word != exp_word) {
        mismatches++;
        if (reported < max_report) {
          const uint32_t a = addr + i * 4u;
          Serial.printf("Mismatch @ 0x%08lX: exp=%08lX got=%08lX\n", (unsigned long)a, (unsigned long)exp_word,
                        (unsigned long)got_word);
          reported++;
        }
      }
    }
  }

  if (mismatch_count_out) *mismatch_count_out = mismatches;
  return mismatches == 0;
}

bool read_program_counter() {
  // This function reads the Program Counter (PC) register to prove we can access
  // core registers while NRST is HIGH.
  //
  // IMPORTANT:
  // - CoreSight core-register reads require the core to be halted.
  // - Therefore, multiple reads will typically return the SAME PC while halted.
  //
  // ARM CoreSight mechanism for reading core registers:
  // 1) Halt the core (write to DHCSR)
  // 2) Write register number to DCRSR (Debug Core Register Selector)
  // 3) Wait for S_REGRDY bit in DHCSR
  // 4) Read value from DCRDR (Debug Core Register Data)
  //
  // Source: ARM Cortex-M0+ Technical Reference Manual, Section 10.2

  Serial.println("Reading Program Counter (PC) register...");
  Serial.println("This test proves we can read core registers after the same connect+halt sequence used by 'r'");

  // Reuse the bench-proven attach sequence (same as 'r'): reset + DP power-up + connect-under-reset + halt.
  if (!connect_and_halt()) {
    Serial.println("ERROR: connect_and_halt failed");
    return false;
  }

  uint32_t dhcsr = 0;
  if (!swd_min::mem_read32_verbose("Read DHCSR status (confirm debug+halt)", DHCSR, &dhcsr)) {
    Serial.println("ERROR: Failed to read DHCSR");
    return false;
  }

  Serial.printf("DHCSR = 0x%08lX (C_DEBUGEN=%d, S_HALT=%d, S_REGRDY=%d)\n",
                (unsigned long)dhcsr,
                (dhcsr & DHCSR_C_DEBUGEN) ? 1 : 0,
                (dhcsr & DHCSR_S_HALT) ? 1 : 0,
                (dhcsr & DHCSR_S_REGRDY) ? 1 : 0);

  if ((dhcsr & DHCSR_S_HALT) == 0) {
    Serial.println("WARN: Core did not report HALT; core-register access may fail");
  }

  Serial.println("Reading PC (R15) 5 times while halted...");
  
  uint32_t pc_values[5];
  bool all_reads_ok = true;
  
  for (int i = 0; i < 5; i++) {
    // Write PC register number (15) to DCRSR to request a read
    // Bit 16 = 0 for read, bits [4:0] = register number
    const uint32_t dcrsr_read_pc = REGNUM_PC;  // Read PC (R15)
    if (!swd_min::mem_write32_verbose("Select CPU register number (DCRSR) to read PC (R15)", DCRSR, dcrsr_read_pc)) {
      Serial.printf("ERROR: Failed to write DCRSR (iteration %d)\n", i);
      all_reads_ok = false;
      break;
    }
    
    // Wait for S_REGRDY in DHCSR (indicates register transfer complete)
    bool ready = false;
    for (int wait = 0; wait < 200; wait++) {
      if (!swd_min::mem_read32(DHCSR, &dhcsr)) {
        Serial.printf("ERROR: Failed to read DHCSR (iteration %d)\n", i);
        all_reads_ok = false;
        break;
      }
      if (dhcsr & DHCSR_S_REGRDY) {
        ready = true;
        break;
      }
      delayMicroseconds(10);
    }
    
    if (!ready) {
      Serial.printf("ERROR: S_REGRDY timeout (iteration %d, DHCSR=0x%08lX)\n", i, (unsigned long)dhcsr);
      all_reads_ok = false;
      break;
    }
    
    // Read the PC value from DCRDR
    uint32_t pc = 0;
    if (!swd_min::mem_read32_verbose("Read CPU register value (DCRDR)", DCRDR, &pc)) {
      Serial.printf("ERROR: Failed to read DCRDR (iteration %d)\n", i);
      all_reads_ok = false;
      break;
    }
    
    pc_values[i] = pc;
    Serial.printf("  Read %d: PC = 0x%08lX\n", i + 1, (unsigned long)pc);

    // Extra sanity check (non-fatal): try reading the memory word containing the current PC.
    // This helps distinguish a plausible internal ROM/flash address from a bogus value.
    if (i == 0) {
      const uint32_t pc_aligned = pc & ~0x3u;
      uint32_t instr_word = 0;
      if (swd_min::mem_read32_verbose("Sanity: read 32-bit word at PC-aligned address", pc_aligned, &instr_word)) {
        Serial.printf("  Word @ PC(align) 0x%08lX = 0x%08lX\n", (unsigned long)pc_aligned,
                      (unsigned long)instr_word);
      } else {
        Serial.println("  WARN: Could not read memory at PC address (sanity check)");
      }
    }
  }
  
  if (!all_reads_ok) {
    Serial.println("FAIL: Could not complete all PC reads");
    return false;
  }
  
  Serial.println("\nAnalysis:");

  // Check if PC values are changing. While halted, this should normally be NO.
  bool pc_changed = false;
  for (int i = 1; i < 5; i++) {
    if (pc_values[i] != pc_values[0]) {
      pc_changed = true;
      break;
    }
  }

  // Check if PC is in valid flash range (0x08000000 - 0x08010000)
  bool pc_in_flash = true;
  for (int i = 0; i < 5; i++) {
    if (pc_values[i] < FLASH_BASE || pc_values[i] >= (FLASH_BASE + FLASH_SIZE_BYTES)) {
      pc_in_flash = false;
      break;
    }
  }
  
  Serial.printf("  PC changed between reads: %s\n", pc_changed ? "YES" : "NO (expected while halted)");
  Serial.printf("  PC in main flash range:   %s\n", pc_in_flash ? "YES (0x08000000-0x08010000)" : "NO (may be ROM/system memory)");
  
  // Check if all PC values are 0 (indicates core may not be running or debug not working)
  bool all_zero = true;
  for (int i = 0; i < 5; i++) {
    if (pc_values[i] != 0) {
      all_zero = false;
      break;
    }
  }
  
  if (all_zero) {
    Serial.println("  WARNING: All PC values are 0x00000000 - core may not be running");
    Serial.println("           This is normal for a completely blank/unprogrammed chip");
  }
  
  Serial.println("\n=== RESULT ===");
  Serial.println("SUCCESS: Read PC via CoreSight (DCRSR/DCRDR) with NRST HIGH");
  Serial.println("(Core left halted by connect_and_halt())");
  
  return true;
}

bool prepare_target_for_normal_run() {
  // Rationale:
  // - During programming/debug we may have left the core halted via DHCSR.C_HALT.
  // - We may also have enabled vector-catch-on-reset (DEMCR.VC_CORERESET) as part
  //   of the connect-under-reset recovery flow.
  // Either of these can prevent the application firmware from running normally
  // after a simple NRST pulse.
  //
  // Clear VC_CORERESET and clear the halt request, but keep debug enabled so we
  // can still re-attach quickly if needed.

  // Best-effort: clear vector catch on reset.
  uint32_t demcr = 0;
  if (!swd_min::mem_read32(DEMCR, &demcr)) return false;
  demcr &= ~DEMCR_VC_CORERESET;
  if (!swd_min::mem_write32(DEMCR, demcr)) return false;

  // Clear C_HALT (bit1) while keeping C_DEBUGEN (bit0) set.
  // Writes to DHCSR require the DBGKEY in upper 16 bits.
  static constexpr uint32_t DHCSR_C_DEBUGEN_ONLY = DHCSR_DBGKEY | DHCSR_C_DEBUGEN;
  if (!swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_ONLY)) return false;

  // Optional: read back for diagnostics when verbose.
  if (verbose()) {
    uint32_t dhcsr = 0;
    (void)swd_min::mem_read32_verbose("Read DHCSR after clearing C_HALT", DHCSR, &dhcsr);
  }

  return true;
}

} // namespace stm32g0_prog
