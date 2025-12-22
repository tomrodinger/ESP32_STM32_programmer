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

- RM0444 is the authoritative source.
- For implementation-ready numeric values (addresses, bit positions, and key constants), we can also rely on the **ST CMSIS device header** for STM32G031 and the **ST HAL flash header**, both of which are derived from RM0444.
  - CMSIS device header: [`docs/stm32g031xx.h`](docs/stm32g031xx.h:1)
  - HAL flash header: [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:1)

## Scope / assumptions

- We only need **full chip erase of main flash** (not partial/page erase).
- Target: **STM32G031** (single-bank flash device in STM32G0 family).
- Host: ESP32-S3 bit-banging SWD and doing memory-mapped register access (see [`swd_min::mem_write32()`](src/swd_min.h:69)).
- The SWD attach strategy currently holds NRST low during initial attach (see [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392)).

## Confirmed register map and bitfields (implementation-ready)

Everything in this section is taken directly from ST headers already in this repo:

- [`docs/stm32g031xx.h`](docs/stm32g031xx.h:240) (STM32G031 CMSIS device header)
- [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:140) (STM32G0 HAL flash header)

### Flash registers base address (FLASH_R)

From the STM32G031 memory map:

- `PERIPH_BASE = 0x40000000` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:559))
- `AHBPERIPH_BASE = PERIPH_BASE + 0x00020000 = 0x40020000` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:567))
- `FLASH_R_BASE = AHBPERIPH_BASE + 0x00002000 = 0x40022000` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:604))

So:

- **FLASH register base address** (FLASH_R): `0x40022000`

### Flash register offsets

The FLASH register block layout is defined by `FLASH_TypeDef`:

- `FLASH_ACR` offset `0x00` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:250))
- `FLASH_KEYR` offset `0x08` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:252))
- `FLASH_OPTKEYR` offset `0x0C` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:253))
- `FLASH_SR` offset `0x10` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:254))
- `FLASH_CR` offset `0x14` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:255))
- `FLASH_OPTR` offset `0x20` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:258))

Computed absolute addresses (FLASH_R_BASE + offset):

- `FLASH_ACR  = 0x40022000 + 0x00 = 0x40022000`
- `FLASH_KEYR = 0x40022000 + 0x08 = 0x40022008`
- `FLASH_OPTKEYR = 0x40022000 + 0x0C = 0x4002200C`
- `FLASH_SR  = 0x40022000 + 0x10 = 0x40022010`
- `FLASH_CR  = 0x40022000 + 0x14 = 0x40022014`
- `FLASH_OPTR = 0x40022000 + 0x20 = 0x40022020`

### Flash unlock keys (confirmed)

From ST HAL:

- `FLASH_KEY1 = 0x45670123` ([`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:158))
- `FLASH_KEY2 = 0xCDEF89AB` ([`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:158))

### FLASH_CR bits used for mass erase (confirmed)

From the bit definitions:

- `FLASH_CR_PG` bit **0** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2482))
- `FLASH_CR_PER` bit **1** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2485))
- `FLASH_CR_MER1` bit **2** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2488))
- `FLASH_CR_STRT` bit **16** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2494))
- `FLASH_CR_LOCK` bit **31** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2521))

STM32G031 is single-bank, and the STM32G031 header defines only `MER1` (no `MER2`).

### FLASH_SR bits used for erase (confirmed)

Busy and completion:

- `FLASH_SR_EOP` bit **0** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2441))
- `FLASH_SR_BSY1` bit **16** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2474))

Error flags (all must be checked and cleared as needed):

- `FLASH_SR_OPERR` bit **1** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2444))
- `FLASH_SR_PROGERR` bit **3** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2447))
- `FLASH_SR_WRPERR` bit **4** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2450))
- `FLASH_SR_PGAERR` bit **5** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2453))
- `FLASH_SR_SIZERR` bit **6** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2456))
- `FLASH_SR_PGSERR` bit **7** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2459))
- `FLASH_SR_MISERR` bit **8** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2462))
- `FLASH_SR_FASTERR` bit **9** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2465))
- `FLASH_SR_RDERR` bit **14** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2468))
- `FLASH_SR_OPTVERR` bit **15** ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2471))

`FLASH_SR_CFGBSY` bit **18** exists as well (config busy) ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2477)); treat it like a busy condition for option-byte operations.

### How to clear flags (W1C confirmed)

ST HAL’s clear-flag macro shows how status flags are cleared:

- For SR flags, it does: `FLASH->SR = (1uL << (bit_index))` ([`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:786))

So: **clear `FLASH_SR_*` flags by writing 1 to the corresponding bit in `FLASH_SR`**.

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

- **Power supply** must be stable and within datasheet limits (STM32G031 VDD operating range is 1.7–3.6 V per datasheet).
- Avoid brown-out resets during erase (BOR/PVD configuration matters in production).
- If a watchdog (IWDG/WWDG) could be active, ensure it cannot reset the MCU during erase (either disable it earlier in the flow, or refresh it from a RAM-resident loop).

NRST note (important for this project):

- Our SWD attach currently holds NRST low during early debug attach (see [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392)). This is known-good for reading DP IDCODE.
- It is **not guaranteed** (without an RM0444 quote) that flash erase operations are valid while NRST remains asserted. For safety, plan on **releasing NRST high** before performing flash operations (erase/program), then halting the core.

### 1) Connect, power-up debug, and halt core

- Perform SWD line reset / JTAG-to-SWD and DP init/power-up (ADIv5 CTRL/STAT handshake).
  - In this repo: [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:423)
- Halt the core using DHCSR (enable debug + halt) so the CPU is not executing from flash.
  - In this repo: [`stm32g0_prog::connect_and_halt()`](src/stm32g0_prog.cpp:76)

Why halting matters:

- Flash erase stalls/blocks flash reads; if the CPU is fetching instructions from flash, you can deadlock or get unpredictable behavior.
- Even with the CPU halted, some peripherals/DMAs could still access flash; production flow should avoid that.

### 2) Ensure flash controller is idle

- Read FLASH status register (FLASH_SR) and confirm **BSY1 == 0** ([`FLASH_SR_BSY1`](docs/stm32g031xx.h:2474)).
- If BSY stays set beyond a reasonable timeout, treat it as a failure.

Implementation note:

- Our current code polls a “busy” bit in [`wait_flash_not_busy()`](src/stm32g0_prog.cpp:41), but the bit mask used must be verified against RM0444.

### 3) Clear FLASH_SR status flags (especially errors)

Before starting *any* new erase:

- Clear `EOP` (end-of-operation) if set.
- Clear all error flags by W1C writes.

Concrete list of SR flags to clear for STM32G031 (from [`docs/stm32g031xx.h`](docs/stm32g031xx.h:2440)):

- `EOP`, `OPERR`, `PROGERR`, `WRPERR`, `PGAERR`, `SIZERR`, `PGSERR`, `MISERR`, `FASTERR`, `RDERR`, `OPTVERR`.

Implementation detail:

- You may clear them one-by-one (write `1<<bit`) as ST HAL does ([`__HAL_FLASH_CLEAR_FLAG`](docs/stm32g0xx_hal_flash.h:786)), or clear multiple at once by writing an OR-mask to `FLASH_SR`.

Why do this:

- Some flags can be **sticky**, and leaving them set can cause later operations to be blocked or hard to diagnose.

### 4) Unlock flash control registers

- If FLASH_CR indicates LOCK is set, unlock via the key sequence:
  1. Write KEY1 to FLASH_KEYR.
  2. Write KEY2 to FLASH_KEYR.
  3. Re-read FLASH_CR and confirm LOCK cleared.

The STM32G0 flash uses the canonical ST key pair (documented in RM0444 and already used in our code):

- KEY1 = `0x45670123` ([`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:158))
- KEY2 = `0xCDEF89AB` ([`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:158))

In this repo: [`flash_unlock()`](src/stm32g0_prog.cpp:52)

Important robustness detail:

- Do not interleave unrelated register accesses between KEY1 and KEY2. Perform the two writes back-to-back.

### 5) Configure mass erase (STM32G031: single bank)

- Ensure no page erase/program bits are set in `FLASH_CR` (clear `PG`/`PER` etc.) ([`FLASH_CR_PG`](docs/stm32g031xx.h:2482), [`FLASH_CR_PER`](docs/stm32g031xx.h:2485)).
- Set the **mass erase** request bit for Bank 1: `MER1` ([`FLASH_CR_MER1`](docs/stm32g031xx.h:2488)).
  - STM32G031 is single-bank, so `MER2` (if present on larger parts) is not used.

### 6) Start the mass erase

- Set the start bit `STRT` ([`FLASH_CR_STRT`](docs/stm32g031xx.h:2494)).
- After this write, BSY should become 1 soon after.

### 7) Poll for completion and validate outcome

- Poll FLASH_SR.BSY until it becomes 0 (timeout large enough for worst-case erase time).
- After BSY clears:
  - Check EOP indicates successful completion.
    - Bench note for this project: we have observed cases where `FLASH_SR` reads back as `0x00000000` at completion (so `EOP` appears clear) even though flash contents are erased (verified by reading back `0xFFFFFFFF` at `0x08000000`).
    - Therefore, treat `EOP==0` as a warning and always verify flash contents.
  - Check error flags; if any are set (WRP/protection, operation error, etc.), treat erase as failed.

Protection-related failure modes to account for:

- **Write protection (WRP)** option bytes can cause mass erase to fail for protected ranges; in that case you typically see a WRP-related error in FLASH_SR.
- **Readout protection (RDP)** levels change what debug can do:
  - RDP Level 0: normal debug erase is allowed.
  - RDP Level 1: debug access to flash is restricted; lowering RDP back to 0 triggers an *automatic mass erase* as part of the transition.
  - RDP Level 2: debug access is disabled/irreversible for production (no SWD erase).

Important completeness note:

- The exact “symptom” of RDP/WRP/PCROP blocking mass erase (e.g. which FLASH_SR bits set, whether AHB-AP access faults, etc.) must be confirmed from ST docs / RM0444; don’t pattern-match on a single returned value.

### 8) Clear completion + error flags

- Clear EOP and any error flags (write-1-to-clear bits in FLASH_SR).
- (Optional but recommended) Re-read FLASH_SR to confirm flags cleared.

### 9) Clear erase bits and re-lock flash

- Clear `MER1` and `STRT` in `FLASH_CR`.
- Set `LOCK` to re-lock control ([`FLASH_CR_LOCK`](docs/stm32g031xx.h:2521)).

### 10) Verify memory is erased

- Read several 32-bit words across the flash range (start/middle/end) and confirm `0xFFFFFFFF`.
- For production robustness, at least verify:
  - vector table area at 0x0800_0000
  - one word in each quarter of the flash region

## Notes on caches / prefetch

STM32G0 flash includes memory acceleration features controlled by `FLASH_ACR` (see ST training deck “STM32G0 – Memory Flash”). Those features are primarily relevant when the **CPU resumes executing from flash** after a program/erase.

Engineering best practice pattern (optional; verify exact bits in RM0444 before doing it):

- Disable prefetch / instruction cache before erase/program.
- After operation: reset (invalidate) cache, then re-enable.

This is most relevant if the target will *resume execution* immediately after programming. For a minimal “mass erase while halted” proof, cache/prefetch manipulation is not strictly required.

## Comparison vs current implementation in this repo (what to check)

Current mass erase implementation: [`stm32g0_prog::flash_mass_erase()`](src/stm32g0_prog.cpp:105)

Items that must be verified/updated when we implement the “final” production-safe erase:

1. **BSY bit mask**: our code currently defines `FLASH_SR_BSY = (1u << 16)` in [`src/stm32g0_prog.cpp`](src/stm32g0_prog.cpp:31). This matches `FLASH_SR_BSY1_Pos = 16` in [`docs/stm32g031xx.h`](docs/stm32g031xx.h:2474).
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
