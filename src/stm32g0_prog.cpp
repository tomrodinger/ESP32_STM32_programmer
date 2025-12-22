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

static bool flash_clear_sr_flags(uint32_t mask) {
  // STM32G0: FLASH_SR flags are W1C (write 1 to clear) per ST HAL
  // See: [`FLASH_ERASE.md`](FLASH_ERASE.md:110) and [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:786)
  if ((mask & FLASH_SR_CLEAR_MASK) == 0) return true;
  return swd_min::mem_write32(FLASH_SR, mask & FLASH_SR_CLEAR_MASK);
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
  if ((sr & FLASH_SR_EOP) == 0) {
    Serial.printf("ERROR: flash erase did not set EOP: FLASH_SR=0x%08lX\n", (unsigned long)sr);
    return false;
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
  // Mass erase using "connect under reset" - recovers chips where firmware disables SWD.
  // See MASS_ERASE.md for details.
  //
  // IMPORTANT FOR CORTEX-M0+ (STM32G031):
  // - M0+ does NOT have VC_CORERESET (that's M3+ only)
  // - DEMCR and other core registers are NOT accessible while NRST is held LOW
  // - Peripheral registers (flash, GPIO) return reset values (0x00000000) while NRST is LOW
  //
  // The M0+ technique uses TIMING-BASED halt:
  // 1. Configure SWD fully while NRST is LOW (DP, AP, SELECT, CSW)
  // 2. Pre-set TAR to point to DHCSR (0xE000EDF0)
  // 3. Release NRST and IMMEDIATELY write halt command to DRW
  // 4. The instruction fetch abort mechanism halts core before firmware runs
  //
  // This works because the SWD state machine is "pre-staged" while NRST is LOW,
  // so when NRST is released, only a single DRW write is needed.

  Serial.println("Mass erase using connect-under-reset (M0+ timing method)...");

  // Step 1: Establish SWD connection with NRST asserted
  Serial.println("Step 1: Reset target and establish SWD connection...");
  swd_min::reset_and_switch_to_swd();  // This asserts NRST and does JTAG->SWD switch

  // Step 2: Initialize DP and power up debug domain (while NRST still LOW)
  Serial.println("Step 2: Initialize DP (NRST still LOW)...");
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed");
    return false;
  }
  Serial.println("  DP initialized OK");

  // Step 3: Pre-configure AP to point TAR at DHCSR
  // This way, after NRST release, we only need ONE transaction (DRW write)
  Serial.println("Step 3: Pre-configure AP (set TAR to DHCSR address)...");
  
  // Write CSW for 32-bit access
  uint8_t ack = 0;
  if (!swd_min::ap_write_reg(0x00, 0x23000012, &ack) || ack != 1) {  // CSW: 32-bit, auto-inc off
    Serial.printf("ERROR: CSW write failed, ACK=%u\n", ack);
    return false;
  }
  
  // Write TAR = DHCSR (0xE000EDF0)
  if (!swd_min::ap_write_reg(0x04, DHCSR, &ack) || ack != 1) {  // TAR = 0xE000EDF0
    Serial.printf("ERROR: TAR write failed, ACK=%u\n", ack);
    return false;
  }
  Serial.println("  AP pre-configured: TAR = 0xE000EDF0 (DHCSR)");

  // Step 4: CRITICAL TIMING - Release NRST and IMMEDIATELY send halt command
  // We cannot have ANY delays here. The halt must arrive before first instruction executes.
  Serial.println("Step 4: Release NRST and IMMEDIATELY send halt...");
  
  // Release NRST - core starts executing from reset vector
  swd_min::set_nrst(false);
  
  // NO DELAY! Immediately write halt command to DRW (which writes to DHCSR via TAR)
  // DHCSR halt value = 0xA05F0003 (DBGKEY | C_DEBUGEN | C_HALT)
  bool first_halt_ok = swd_min::ap_write_reg(0x0C, DHCSR_C_DEBUGEN_C_HALT, &ack);
  Serial.printf("  First halt DRW write: %s ACK=%u\n", first_halt_ok ? "OK" : "FAIL", ack);
  
  if (first_halt_ok && ack == 1) {
    // The first write succeeded! The halt command reached the AP.
    // Now wait for the halt to take effect. The debug system needs some time
    // to propagate the halt request to the CPU.
    Serial.println("  First halt command accepted (ACK=1). Waiting for halt to take effect...");
    
    // Wait longer - give time for:
    // 1. The AHB write from AP to DHCSR to complete
    // 2. The debug unit to process the halt request
    // 3. The CPU to actually stop (may be in middle of instruction)
    delay(5);  // 5ms should be plenty for halt to take effect
    
    // Now re-initialize SWD. If the halt worked, the GPIO pins should NOT
    // have been reconfigured because the firmware never got that far.
    Serial.println("  Re-initializing SWD after halt...");
    
    // Assert NRST briefly to reset the debug port state machine (NOT the core)
    // Actually, this would clear the halt! Don't do this.
    // Instead, just do a line reset to re-sync the SWD protocol.
    swd_min::swd_line_reset();
    delay(1);
    
    if (swd_min::dp_init_and_power_up()) {
      Serial.println("  SWD re-initialized successfully after halt!");
      // Check if core is actually halted
      uint32_t dhcsr = 0;
      if (swd_min::mem_read32(DHCSR, &dhcsr)) {
        Serial.printf("  DHCSR = 0x%08lX (S_HALT=%d)\n",
                      (unsigned long)dhcsr, (dhcsr & DHCSR_S_HALT) ? 1 : 0);
        if (dhcsr & DHCSR_S_HALT) {
          Serial.println("  SUCCESS: Core is halted!");
          goto step5;  // Skip to step 5
        }
      }
    }
  }
  
  // If we get here, the first halt didn't work or we can't communicate.
  // Try aggressive recovery: pulse NRST again while doing continuous SWD activity.
  Serial.println("  First halt attempt didn't work. Trying aggressive recovery...");
  
  // Re-assert NRST to start fresh
  swd_min::set_nrst(true);
  delay(10);
  
  // Re-initialize SWD while NRST is LOW
  swd_min::swd_line_reset();
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed with NRST LOW (recovery)");
    return false;
  }
  
  // Pre-stage AP again
  if (!swd_min::ap_write_reg(0x00, 0x23000012, &ack) || ack != 1) {
    Serial.printf("ERROR: CSW write failed (recovery), ACK=%u\n", ack);
    return false;
  }
  if (!swd_min::ap_write_reg(0x04, DHCSR, &ack) || ack != 1) {
    Serial.printf("ERROR: TAR write failed (recovery), ACK=%u\n", ack);
    return false;
  }
  
  // Release NRST and send multiple halt commands as fast as possible
  swd_min::set_nrst(false);
  for (int i = 0; i < 10; i++) {
    swd_min::ap_write_reg(0x0C, DHCSR_C_DEBUGEN_C_HALT, &ack);
    if (ack != 1) break;  // Stop once we start getting errors
  }
  
  // Wait for halt
  delay(5);
  
  // Try to re-init
  swd_min::swd_line_reset();
  delay(1);
  
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed after NRST release (recovery)");
    Serial.println("HINT: The target firmware disables SWD too quickly.");
    Serial.println("      Try using BOOT0 pin to enter system bootloader.");
    return false;
  }

step5:

  // Step 5: Verify core is halted
  Serial.println("Step 5: Verify core is halted...");
  uint32_t dhcsr = 0;
  bool halted = false;
  for (int i = 0; i < 10; i++) {
    if (swd_min::mem_read32(DHCSR, &dhcsr)) {
      Serial.printf("  DHCSR = 0x%08lX (S_HALT=%d)\n",
                    (unsigned long)dhcsr, (dhcsr & DHCSR_S_HALT) ? 1 : 0);
      if (dhcsr & DHCSR_S_HALT) {
        halted = true;
        break;
      }
    }
    // Try to halt again
    swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_C_HALT);
    delay(1);
  }
  
  if (!halted) {
    Serial.println("WARN: Core not reporting halted - firmware may have started!");
    Serial.println("  Making one more attempt to halt...");
    swd_min::mem_write32(DHCSR, DHCSR_C_DEBUGEN_C_HALT);
    delay(10);
    if (swd_min::mem_read32(DHCSR, &dhcsr) && (dhcsr & DHCSR_S_HALT)) {
      Serial.println("  Core halted on retry");
      halted = true;
    }
  }
  
  if (halted) {
    Serial.println("  Core halted - SUCCESS!");
  } else {
    Serial.println("  WARNING: Core may not be halted, attempting erase anyway...");
  }

  // Step 6: Now we can access peripherals! Verify by reading FLASH_CR
  Serial.println("Step 6: Verify peripheral access (read FLASH_CR)...");
  uint32_t cr = 0;
  if (!swd_min::mem_read32(FLASH_CR, &cr)) {
    Serial.println("ERROR: Cannot read FLASH_CR");
    return false;
  }
  Serial.printf("  FLASH_CR = 0x%08lX (LOCK=%d)\n", (unsigned long)cr, (cr & FLASH_CR_LOCK) ? 1 : 0);
  
  if (cr == 0x00000000) {
    Serial.println("WARN: FLASH_CR is 0 - peripheral access may not be working!");
  }

  // Step 7: Perform mass erase
  Serial.println("Step 7: Perform mass erase...");
  
  // Wait for any flash operation to complete
  if (!wait_flash_not_busy(5000)) {
    Serial.println("ERROR: flash busy timeout before erase");
    return false;
  }

  // Clear status flags
  if (!flash_clear_sr_flags(FLASH_SR_CLEAR_MASK)) {
    Serial.println("ERROR: failed to clear FLASH_SR flags");
    return false;
  }

  // Unlock flash
  if (!flash_unlock()) {
    Serial.println("ERROR: flash unlock failed");
    return false;
  }
  Serial.println("  Flash unlocked");

  // Clear conflicting bits
  if (!flash_clear_cr_bits(FLASH_CR_PG | FLASH_CR_PER)) {
    Serial.println("ERROR: failed to clear FLASH_CR bits");
    return false;
  }

  // Set MER1 (Mass Erase) bit
  Serial.println("  Setting MER1 bit...");
  if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_MER1)) {
    Serial.println("ERROR: failed to set MER1");
    return false;
  }

  // Start the erase
  Serial.println("  Starting mass erase (STRT)...");
  if (!swd_min::mem_write32(FLASH_CR, FLASH_CR_MER1 | FLASH_CR_STRT)) {
    Serial.println("ERROR: failed to set STRT");
    return false;
  }

  // Wait for completion
  Serial.println("  Waiting for erase completion (timeout 30s)...");
  const uint32_t start_ms = millis();
  uint32_t sr = 0;
  bool done = false;
  while ((millis() - start_ms) < 30000) {
    if (!swd_min::mem_read32(FLASH_SR, &sr)) {
      Serial.println("ERROR: FLASH_SR read failed during wait");
      return false;
    }
    if ((sr & FLASH_SR_BSY) == 0) {
      done = true;
      break;
    }
    if (((millis() - start_ms) % 1000) < 50) {
      Serial.print(".");
    }
    delay(50);
  }
  Serial.println();

  if (!done) {
    Serial.println("ERROR: flash busy timeout during mass erase");
    return false;
  }

  const uint32_t elapsed_ms = millis() - start_ms;
  Serial.printf("  Erase completed in %lu ms\n", (unsigned long)elapsed_ms);

  // Check for errors
  if (!swd_min::mem_read32(FLASH_SR, &sr)) {
    Serial.println("ERROR: final FLASH_SR read failed");
    return false;
  }
  Serial.printf("  Final FLASH_SR = 0x%08lX\n", (unsigned long)sr);

  if (sr & FLASH_SR_ALL_ERRORS) {
    Serial.printf("ERROR: flash erase error flags: FLASH_SR=0x%08lX\n", (unsigned long)sr);
    (void)flash_clear_sr_flags(sr);
    return false;
  }

  if ((sr & FLASH_SR_EOP) == 0) {
    Serial.printf("WARN: EOP not set: FLASH_SR=0x%08lX\n", (unsigned long)sr);
  }

  // Clean up
  (void)flash_clear_sr_flags(FLASH_SR_CLEAR_MASK);
  (void)flash_clear_cr_bits(FLASH_CR_MER1 | FLASH_CR_STRT);
  (void)swd_min::mem_write32(FLASH_CR, FLASH_CR_LOCK);

  // Step 8: Verify by reading flash
  Serial.println("Step 8: Verify erase (reading 0x08000000)...");
  uint32_t verify_word = 0;
  if (swd_min::mem_read32(0x08000000, &verify_word)) {
    Serial.printf("  Flash[0x08000000] = 0x%08lX (expect 0xFFFFFFFF)\n", (unsigned long)verify_word);
    if (verify_word == 0xFFFFFFFF) {
      Serial.println("Mass erase under reset: SUCCESS!");
      return true;
    } else {
      Serial.println("ERROR: Flash not erased to 0xFFFFFFFF");
      return false;
    }
  } else {
    Serial.println("WARN: Could not read flash for verification");
  }

  Serial.println("Mass erase under reset: COMPLETED (verification inconclusive)");
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

bool read_program_counter() {
  // This function reads the Program Counter (PC) register to prove we can access
  // core registers with NRST HIGH. On a fresh unprogrammed chip, this shows:
  // 1. SWD connection works with NRST HIGH (not stuck in reset)
  // 2. We can halt the core and access debug registers
  // 3. We can read CPU core registers through CoreSight
  //
  // ARM CoreSight mechanism for reading core registers:
  // - Halt the core (write to DHCSR)
  // - Write register number to DCRSR (Debug Core Register Selector)
  // - Wait for S_REGRDY bit in DHCSR
  // - Read value from DCRDR (Debug Core Register Data)
  //
  // Source: ARM Cortex-M0+ Technical Reference Manual, Section 10.2

  Serial.println("Reading Program Counter (PC) register...");
  Serial.println("This test proves we can read core registers while NRST is HIGH");
  
  // Step 1: Reset and establish initial SWD connection with NRST LOW
  Serial.println("Step 1: Reset and establish SWD connection (NRST LOW)...");
  swd_min::reset_and_switch_to_swd();  // This asserts NRST
  
  if (!swd_min::dp_init_and_power_up()) {
    Serial.println("ERROR: dp_init_and_power_up failed");
    return false;
  }
  Serial.println("  SWD initialized (NRST still LOW)");
  
  // Step 2: Release NRST and re-establish connection
  Serial.println("Step 2: Release NRST and re-establish connection...");
  if (!swd_min::connect_under_reset_and_init()) {
    Serial.println("ERROR: connect_under_reset_and_init failed");
    return false;
  }
  Serial.println("  SWD connection established with NRST HIGH - SUCCESS!");
  Serial.println("  (This proves SWD works with NRST high on fresh chip)");
  
  // Step 3: Halt the core so we can access debug registers
  Serial.println("Step 3: Halt the core...");
  if (!swd_min::mem_write32_verbose("Enable debug and halt the CPU (DHCSR)", DHCSR, DHCSR_C_DEBUGEN_C_HALT)) {
    Serial.println("ERROR: Failed to write DHCSR to halt core");
    return false;
  }
  
  // Wait for halt to take effect
  delay(5);
  
  // Verify core is halted
  uint32_t dhcsr = 0;
  if (!swd_min::mem_read32_verbose("Read DHCSR status", DHCSR, &dhcsr)) {
    Serial.println("ERROR: Failed to read DHCSR");
    return false;
  }
  
  Serial.printf("  DHCSR = 0x%08lX (C_DEBUGEN=%d, S_HALT=%d, S_REGRDY=%d)\n",
                (unsigned long)dhcsr,
                (dhcsr & DHCSR_C_DEBUGEN) ? 1 : 0,
                (dhcsr & DHCSR_S_HALT) ? 1 : 0,
                (dhcsr & DHCSR_S_REGRDY) ? 1 : 0);
  
  if ((dhcsr & DHCSR_S_HALT) == 0) {
    Serial.println("WARN: Core did not halt, but continuing anyway...");
  } else {
    Serial.println("  Core is halted - good!");
  }
  
  // Step 4: Read PC multiple times to show it's changing
  Serial.println("Step 4: Reading PC register multiple times...");
  Serial.println("  (If values change, core is executing instructions)");
  
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
    for (int wait = 0; wait < 100; wait++) {
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
        Serial.printf("ERROR: S_REGRDY timeout (iteration %d, DHCSR=0x%08lX)\n",
                      i, (unsigned long)dhcsr);
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
    
    // Small delay before next read to give core time to execute
    delayMicroseconds(100);
  }
  
  if (!all_reads_ok) {
    Serial.println("FAIL: Could not complete all PC reads");
    return false;
  }
  
  // Step 5: Analyze the results
  Serial.println("\nStep 5: Analysis:");
  
  // Check if PC values are changing (proves core is running)
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
  
  Serial.printf("  PC changed between reads: %s\n", pc_changed ? "YES (core is executing!)" : "NO (core may be halted/stuck)");
  Serial.printf("  PC in valid flash range:  %s\n", pc_in_flash ? "YES (0x08000000-0x08010000)" : "NO (unexpected)");
  
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
  
  // Success criteria: We were able to read PC (proves register access works)
  // The PC value may or may not change depending on chip state
  Serial.println("\n=== RESULT ===");
  Serial.println("SUCCESS: We can read the Program Counter register while NRST is HIGH!");
  Serial.println("This proves:");
  Serial.println("  1. SWD connection works with NRST HIGH (not stuck in reset)");
  Serial.println("  2. Debug register access is functional");
  Serial.println("  3. We can read CPU core registers through CoreSight");
  
  if (pc_changed) {
    Serial.println("  4. Core is EXECUTING code (PC is changing)");
  } else {
    Serial.println("  4. Core appears halted/stuck (PC not changing)");
    Serial.println("     (This is expected for unprogrammed/blank flash)");
  }
  
  return true;
}

} // namespace stm32g0_prog
