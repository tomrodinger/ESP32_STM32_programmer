# STM32G031 (STM32G0x1) — Full Flash (Mass) Erase Procedure (via SWD)

This document captures the *recommended/expected* sequence to perform a **full (mass) erase** of STM32G031 internal flash when you have **debug access over SWD** (SWCLK/SWDIO/NRST) and can read/write target memory-mapped registers through the AHB-AP.

It is written to be used as a checklist so we can later implement the procedure robustly in firmware.

## Primary references

- ST **RM0444** (STM32G0x1 Reference Manual), FLASH chapter / register descriptions. (Authoritative for register semantics.)
  - URL: https://www.st.com/resource/en/reference_manual/rm0444-stm32g0x1-advanced-armbased-32bit-mcus-stmicroelectronics.pdf
- ST training deck **STM32G0 – Memory Flash** (high-level behavior, cache/prefetch notes).
  - URL: https://www.st.com/resource/en/product_training/STM32G0-Memory-Flash-FLASH.pdf
- STM32G031 datasheet (operating conditions, voltage range).
  - URL: https://www.st.com/resource/en/datasheet/stm32g031c6.pdf

Notes:

- I was not able to reliably fetch RM0444 content from within this workspace due to network transfer issues, so this file intentionally **avoids hard-coding bit numbers/offsets beyond what is already in our code** and instead focuses on the **procedure** and **what to verify** from RM0444 when implementing.

## Scope / assumptions

- We only need **full chip erase of main flash** (not partial/page erase).
- Target: **STM32G031** (single-bank flash device in STM32G0 family).
- Host: ESP32-S3 bit-banging SWD and doing memory-mapped register access (see [`swd_min::mem_write32()`](src/swd_min.h:69)).
- The SWD attach strategy currently holds NRST low during initial attach (see [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392)).

## High-level sequence (what must happen)

1. Establish SWD debug access (DP power-up, AHB-AP working).
2. Put the Cortex-M0+ into a controlled state (halt, prevent flash execution).
3. Ensure the flash controller is idle.
4. Clear any stale flash status flags (esp. error flags) so we can detect new failures.
5. Unlock flash control.
6. Configure **mass erase** (bank 1) and start the operation.
7. Poll for completion (busy clears), then check EOP + error flags.
8. Clear EOP/error flags.
9. Clear erase control bits and re-lock flash.
10. Verify flash is erased (spot-check reads == 0xFF..).

## Detailed step-by-step checklist

### 0) Preconditions / environmental constraints

- **Power supply** must be stable and within datasheet limits (STM32G031 VDD operating range is 1.7–3.6 V; practical robustness for erase/program is best near 3.0–3.6 V).
- Avoid brown-out resets during erase (BOR/PVD configuration matters in production).
- If a watchdog (IWDG/WWDG) could be active, ensure it cannot reset the MCU during erase (either disable it earlier in the flow, or refresh it from a RAM-resident loop).

### 1) Connect, power-up debug, and halt core

- Perform SWD line reset / JTAG-to-SWD and DP init/power-up (ADIv5 CTRL/STAT handshake).
  - In this repo: [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:423)
- Halt the core using DHCSR (enable debug + halt) so the CPU is not executing from flash.
  - In this repo: [`stm32g0_prog::connect_and_halt()`](src/stm32g0_prog.cpp:76)

Why halting matters:

- Flash erase stalls/blocks flash reads; if the CPU is fetching instructions from flash, you can deadlock or get unpredictable behavior.
- Even with the CPU halted, some peripherals/DMAs could still access flash; production flow should avoid that.

### 2) Ensure flash controller is idle

- Read FLASH status register (FLASH_SR) and confirm **BSY == 0**.
- If BSY stays set beyond a reasonable timeout, treat it as a failure.

Implementation note:

- Our current code polls a “busy” bit in [`wait_flash_not_busy()`](src/stm32g0_prog.cpp:41), but the bit mask used must be verified against RM0444.

### 3) Clear FLASH_SR status flags (especially errors)

Before starting *any* new erase:

- Clear EOP (end-of-operation) if set.
- Clear all error flags that are “write 1 to clear” (typical STM32 set: OPERR/PROGERR/WRPERR/… — exact names vary by family; confirm for STM32G0 in RM0444).

Why do this:

- Some flags can be **sticky**, and leaving them set can cause later operations to be blocked or hard to diagnose.

### 4) Unlock flash control registers

- If FLASH_CR indicates LOCK is set, unlock via the key sequence:
  1. Write KEY1 to FLASH_KEYR.
  2. Write KEY2 to FLASH_KEYR.
  3. Re-read FLASH_CR and confirm LOCK cleared.

The STM32G0 flash uses the canonical ST key pair (documented in RM0444 and already used in our code):

- KEY1 = `0x45670123`
- KEY2 = `0xCDEF89AB`

In this repo: [`flash_unlock()`](src/stm32g0_prog.cpp:52)

Important robustness detail:

- Do not interleave unrelated register accesses between KEY1 and KEY2. Perform the two writes back-to-back.

### 5) Configure mass erase (STM32G031: single bank)

- Ensure no page erase/program bits are set in FLASH_CR (clear PER/PG etc.).
- Set the **mass erase** request bit for Bank 1 (commonly named `MER1` in STM32G0).
  - STM32G031 is single-bank, so `MER2` (if present on larger parts) is not used.

### 6) Start the mass erase

- Set the start bit (commonly named `STRT`).
- After this write, BSY should become 1 soon after.

### 7) Poll for completion and validate outcome

- Poll FLASH_SR.BSY until it becomes 0 (timeout large enough for worst-case erase time).
- After BSY clears:
  - Check EOP indicates successful completion.
  - Check error flags; if any are set (WRP/protection, operation error, etc.), treat erase as failed.

Protection-related failure modes to account for:

- **Write protection (WRP)** option bytes can cause mass erase to fail for protected ranges; in that case you typically see a WRP-related error in FLASH_SR.
- **Readout protection (RDP)** levels change what debug can do:
  - RDP Level 0: normal debug erase is allowed.
  - RDP Level 1: debug access to flash is restricted; lowering RDP back to 0 triggers an *automatic mass erase* as part of the transition.
  - RDP Level 2: debug access is disabled/irreversible for production (no SWD erase).

### 8) Clear completion + error flags

- Clear EOP and any error flags (write-1-to-clear bits in FLASH_SR).
- (Optional but recommended) Re-read FLASH_SR to confirm flags cleared.

### 9) Clear erase bits and re-lock flash

- Clear the mass erase request bit(s) (`MER1`) and start bit (`STRT`) in FLASH_CR.
- Set FLASH_CR.LOCK to re-lock control.

### 10) Verify memory is erased

- Read several 32-bit words across the flash range (start/middle/end) and confirm `0xFFFFFFFF`.
- For production robustness, at least verify:
  - vector table area at 0x0800_0000
  - one word in each quarter of the flash region

## Notes on caches / prefetch

STM32G0 flash includes instruction cache and prefetch features (FLASH_ACR). The ST training material calls out that cache/prefetch improve performance but can create coherency issues around flash operations. Recommended safe pattern:

- Disable prefetch / instruction cache before erase/program.
- After operation: reset (invalidate) cache, then re-enable.

This is most relevant if the target will *resume execution* immediately after programming; since we’re halting via SWD and then programming, we should still follow the safest sequence.

## Comparison vs current implementation in this repo (what to check)

Current mass erase implementation: [`stm32g0_prog::flash_mass_erase()`](src/stm32g0_prog.cpp:105)

Items that must be verified/updated when we implement the “final” production-safe erase:

1. **BSY bit mask**: our code currently defines `FLASH_SR_BSY = (1u << 16)` in [`src/stm32g0_prog.cpp`](src/stm32g0_prog.cpp:31). This must be checked against RM0444 for STM32G0, because on many STM32 families BSY is bit 0.
2. **Status flag clearing**: current code does not clear EOP/error flags before/after erase. Add it.
3. **Error handling**: current code only times out on busy; it does not examine FLASH_SR error flags after erase.
4. **Locking**: current code clears MER1/STRT but does not explicitly set FLASH_CR.LOCK at end.
5. **NRST / run-state**: we currently hold NRST low during attach and do not explicitly release it in the connect/erase flow; confirm target state expectations for erase, and whether we want NRST released before starting flash operations.

## Minimal “production-ready” erase algorithm (pseudo)

This is the procedure we should implement (in RAM on target is not applicable here since we are external), using SWD memory-mapped reads/writes:

1. Attach SWD + DP power up.
2. Halt core (DHCSR).
3. Wait BSY==0.
4. Clear FLASH_SR (EOP + all error flags).
5. Unlock (KEY1, KEY2) and confirm LOCK==0.
6. Program FLASH_CR: set MER1.
7. Program FLASH_CR: set MER1|STRT.
8. Poll BSY until 0 (timeout).
9. Read FLASH_SR: if any error flags set → fail; else require EOP.
10. Clear FLASH_SR flags.
11. Clear MER1/STRT in FLASH_CR.
12. Set LOCK in FLASH_CR.
13. Verify flash words == 0xFFFFFFFF.

