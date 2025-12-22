# STM32G031 — Read Flash via SWD (AHB-AP) 

Goal: implement the **simplest possible proof** that we can read STM32G031 internal flash over **SWD (SWCLK/SWDIO/NRST)** using the existing AHB-AP helpers (see [`swd_min::mem_read32()`](src/swd_min.h:70)):

- Read 8 bytes from **0x0800_0000..0x0800_0007** (vector table start)
- Print them to Serial

This doc is written as an **implementation checklist** with **confirmed constants/behaviors** from Arm ADIv5 and ST sources, and calls out anything that is **NOT guaranteed**.

Key improvement vs earlier drafts: wherever possible, numeric constants are now taken directly from **ST CMSIS/HAL headers checked into this repo** (see “ST sources” below), so we’re not depending on “memory” or vague summaries.

## Authoritative sources used

### Arm (ADIv5 / SWD / AP semantics)

- Arm documentation-service ADIv5 materials referenced by Perplexity:
  - https://documentation-service.arm.com/static/5ed643eaca06a95ce53f92aa
  - https://documentation-service.arm.com/static/622222b2e6f58973271ebc21

### ST (STM32G0 / STM32G031)

- RM0444 (STM32G0x1 reference manual, includes STM32G031):
  - https://www.st.com/resource/en/reference_manual/rm0444-stm32g0x1-advanced-armbased-32bit-mcus-stmicroelectronics.pdf
- STM32G0 security training deck (RDP behavior / access matrix):
  - https://www.st.com/content/ccc/resource/training/technical/product_training/group0/68/e5/5c/b5/40/10/43/22/STM32G0-Security-Memories-Protections-MEMPROTECT/files/STM32G0-Security-Memories-Protections-MEMPROTECT.pdf
- STM32G031 datasheet (flash base, operating conditions):
  - https://www.st.com/resource/en/datasheet/stm32g031c6.pdf

#### ST headers vendored into this repo (implementation-ready)

- STM32G031 CMSIS device header: [`docs/stm32g031xx.h`](docs/stm32g031xx.h:1)
- STM32G0 HAL flash header: [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:1)

## What we already have in this repo

- SWD line reset + JTAG→SWD sequence while holding NRST low: [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392)
- DP init + power-up handshake + sticky clear (ABORT): [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:423)
- AHB-AP memory read/write helpers: [`swd_min::mem_read32()`](src/swd_min.cpp:493) / [`swd_min::mem_write32()`](src/swd_min.cpp:482)

## STM32G031 flash address and flash-register base (confirmed from CMSIS header)

From [`docs/stm32g031xx.h`](docs/stm32g031xx.h:552):

- Main flash base: `FLASH_BASE = 0x08000000` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:557))

Flash register block base (FLASH_R) is derived as:

- `PERIPH_BASE = 0x40000000` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:559))
- `AHBPERIPH_BASE = PERIPH_BASE + 0x00020000` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:567))
- `FLASH_R_BASE = AHBPERIPH_BASE + 0x00002000` ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:604))

So:

- FLASH registers base address is **0x40022000**.

## Confirmed ADIv5 DP register addresses used by SWD

Arm SWD uses only address bits A[3:2], yielding these 4 DP addresses (confirmed in ADIv5 materials cited above; also matches our header constants):

| DP register | Address | In repo | Notes |
|---|---:|---|---|
| IDCODE / ABORT | `0x00` | [`swd_min::DP_ADDR_IDCODE`](src/swd_min.h:43), [`swd_min::DP_ADDR_ABORT`](src/swd_min.h:44) | Same address: read returns IDCODE, write goes to ABORT |
| CTRL/STAT | `0x04` | [`swd_min::DP_ADDR_CTRLSTAT`](src/swd_min.h:45) | Power-up request/ack lives here |
| SELECT | `0x08` | [`swd_min::DP_ADDR_SELECT`](src/swd_min.h:46) | Selects AP + bank |
| RDBUFF | `0x0C` | [`swd_min::DP_ADDR_RDBUFF`](src/swd_min.h:47) | Returns result of posted AP reads |

### DP init / power-up handshake (confirmed behavior)

The DP bring-up sequence we should follow:

1. **Clear sticky errors** by writing DP.ABORT.
   - In repo: [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:423) writes DP.ABORT with bits 1..4 set.
2. **Request power-up** by writing DP.CTRL/STAT with **CSYSPWRUPREQ** and **CDBGPWRUPREQ** set.
   - In repo: it writes `(1<<30) | (1<<28)` to CTRL/STAT.
3. **Poll** DP.CTRL/STAT until **CSYSPWRUPACK** and **CDBGPWRUPACK** are set.
   - In repo: it polls for `(cs>>31)&1` and `(cs>>29)&1`.

This is the correct *shape* of the handshake per ADIv5; the exact bit identities are treated as confirmed by the Arm citations above and our current implementation.

## Confirmed AHB-AP register addresses used for memory access

These are the standard MEM-AP bank-0 registers (also already in our repo):

| AP register | Address | In repo |
|---|---:|---|
| CSW | `0x00` | [`swd_min::AP_ADDR_CSW`](src/swd_min.h:50) |
| TAR | `0x04` | [`swd_min::AP_ADDR_TAR`](src/swd_min.h:51) |
| DRW | `0x0C` | [`swd_min::AP_ADDR_DRW`](src/swd_min.h:52) |
| IDR | `0xFC` | [`swd_min::AP_ADDR_IDR`](src/swd_min.h:53) |

### CSW fields we need for reading 2×32-bit words

**Confirmed by Arm ADIv5** (per the citations in the Perplexity research response):

- `CSW.SIZE[2:0] = 0b010` for 32-bit transfers.
- `CSW.AddrInc[5:4] = 0b01` for single auto-increment (TAR += transfer size after each access).

**Important nuance (must be handled explicitly):**

- Arm MEM-AP also contains a **PROT** field (bus access protection). The Perplexity result asserts that some PROT bits must be set for “privileged” access on some targets. This is common in OpenOCD implementations.
- Our current code uses a *minimal* CSW value with only SIZE + AddrInc (see [`swd_min::mem_read32()`](src/swd_min.cpp:493)).

Action item for firmware correctness:

- Start with the current minimal CSW approach (because it works in the simulator and is simplest): [`swd_min::mem_read32()`](src/swd_min.cpp:493).
- If memory reads fail on real hardware, add a second “CSW preset” that includes PROT bits (matching known-good OpenOCD-style CSW) and test.

## Confirmed posted-read semantics (how to get real data)

Arm AP reads are **posted**: a read request returns a stale value; the **real value** is returned by reading DP.RDBUFF in a subsequent transaction.

In our code this is already handled by [`swd_min::ap_read_reg()`](src/swd_min.cpp:456), which:

1. Issues an AP read
2. Then reads DP.RDBUFF to get the actual value

This is why [`swd_min::mem_read32()`](src/swd_min.cpp:493) can simply read DRW once and return correct data.

## Confirmed SWD physical-layer reset + JTAG→SWD sequence

From the cited SWD/JTAG-to-SWD references in the Perplexity research response:

- **Line reset:** drive SWDIO high for **≥ 50 SWCLK cycles**
- **JTAG→SWD sequence:** transmit **0xE79E**, LSB-first (16 bits)
- **Line reset again:** drive SWDIO high for **≥ 50 cycles**
- Optional: a handful of idle cycles afterward

We already implement this approach in [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392).

## STM32G031-specific constraints

### Flash address range / base

- Main flash starts at **0x0800_0000** (datasheet; also `FLASH_BASE` in [`docs/stm32g031xx.h`](docs/stm32g031xx.h:557)).

### RDP levels (read-out protection) and debug reads

From ST’s STM32G0 security training deck (access control matrix) and RM0444:

- **RDP Level 0:** debug reads of flash are permitted.
- **RDP Level 1:** debug reads of protected areas are **not permitted**.
- **RDP Level 2:** debug is effectively disabled / permanently protected.

**What your SWD read attempt will look like at RDP1/RDP2 is NOT guaranteed in one single universal symptom**.

Depending on silicon/debug implementation you might observe any of:

- SWD ACK FAULT on the memory access
- SWD OK but returned data is all `0xFF`
- core-side fault/reset side effects

Because the ST materials emphasize *access is forbidden* (not a specific bus-return pattern), the implementation must treat “can’t read flash” as a generic failure and not pattern-match on returned values.

Implementation-ready: reading the RDP field

- RDP lives in the flash option register field `FLASH_OPTR.RDP` (mask `0xFF` at bit position 0) ([`FLASH_OPTR_RDP`](docs/stm32g031xx.h:2543)).
- `FLASH_OPTR` register offset is `0x20` within FLASH_R (see `FLASH_TypeDef` layout) ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:248)).
- Therefore `FLASH_OPTR` absolute address is `0x40022000 + 0x20 = 0x40022020`.

Note:

- This lets us add a *diagnostic* step: attempt to read `FLASH_OPTR` via AHB-AP and print the low byte. If that fails, treat it like any other access failure.
- The exact semantic mapping of RDP byte values to “L0/L1/L2” should be pulled from RM0444 and/or the ST security training deck; we do not encode those values here.

### Is flash readable while NRST is held low?

This is the critical one for our current wiring strategy.

- Our current attach flow holds NRST low during early SWD operations (see [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392)).
- The Perplexity research summary states:
  - DP registers remain accessible under reset,
  - but **AHB-AP memory transactions while NRST is asserted are not guaranteed** (may be undefined).

Because we do not have a direct, verbatim RM0444 quote here stating “AHB-AP memory access is guaranteed under NRST low”, we must assume **NOT GUARANTEED**.

**Safest recommended sequence for reading flash on STM32G031:**

1. Hold NRST low
2. Do line reset + JTAG→SWD + DP power-up
3. Release NRST high
4. Delay a short time (order of milliseconds) for clocks/bus fabric to come up
5. Halt core (DHCSR)
6. Perform AHB-AP reads of flash

This gives the highest probability of reliable flash reads.

## Implementation checklist: read first 8 flash bytes and print

### Step 0 — Connect SWD and power-up

1. [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392)
2. [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:423)

### Step 1 — Ensure target is not held in reset (recommended)

3. Release NRST (drive high) using [`swd_min::set_nrst()`](src/swd_min.h:38)
   - In our code, NRST is currently left asserted after [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392). We should explicitly release it before memory reads.

### Step 2 — Halt the core

4. Halt core using [`stm32g0_prog::connect_and_halt()`](src/stm32g0_prog.cpp:76)
   - Or, if we want to avoid the extra reset_and_switch call inside connect_and_halt, replicate its DHCSR write sequence directly.

### Step 3 — Read two 32-bit words from flash

5. Read word0 at 0x0800_0000:
   - call [`swd_min::mem_read32()`](src/swd_min.h:70) with `0x08000000`
6. Read word1 at 0x0800_0004:
   - call [`swd_min::mem_read32()`](src/swd_min.h:70) with `0x08000004`

Optional diagnostic read (recommended):

7. Read `FLASH_OPTR` at 0x40022020 and print low byte:
   - helps identify “protection configuration” vs “wiring/protocol failure”
   - address derivation is confirmed above and in [`docs/stm32g031xx.h`](docs/stm32g031xx.h:604)

### Step 4 — Print as 8 bytes

7. Convert (little-endian) and print:
   - bytes 0..3 from word0, bytes 4..7 from word1

### Error handling requirements (must implement)

Even though our higher-level helpers return only `bool`, the underlying SWD transactions have 3 ACK states:

- OK: continue
- WAIT: retry with bounded timeout
- FAULT: clear sticky errors (DP.ABORT), optionally perform line reset, then retry initialization

Our existing SWD layer already exposes ACK strings for IDCODE reads and has a DP.ABORT clear in [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:423), but `mem_read32()` currently collapses everything into true/false.

For a robust `read_flash_first_8_bytes()` routine, we should:

- Add a “verbose” memory read path that returns the last ACK observed.
- On repeated failure:
  - call [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:423) again (re-clear sticky)
  - re-run [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:392)
  - and retry

## Quick sanity expectations (RDP0)

When RDP is Level 0, the first 8 bytes at 0x0800_0000 are the start of the vector table:

- word0: initial SP value (looks like `0x2000xxxx` typically)
- word1: reset handler address (looks like `0x0800xxxx` typically, LSB may be 1 for Thumb)

So if you read values resembling `0x2000....` then `0x0800....`, your read path is very likely correct.

## Cross-check with STM32G0 flash SR/CR bits (why reading is "simple")

Reading flash (unlike erase/program) does *not* require unlocking flash control registers.

The flash control/status definitions that matter for erase/program are still useful as debug signals when something goes wrong:

- `FLASH_SR_RDERR` exists (bit 14) ([`docs/stm32g031xx.h`](docs/stm32g031xx.h:2468)) and can indicate issues related to PCROP reads on devices that support it.

We do **not** currently read/clear these flags in the SWD read path; we only mention them here so you know where to look when diagnosing access issues.

## STM32G0-specific: NRST release resets DP/AP state

**Critical finding (from Perplexity research, Dec 2024):**

On STM32G0, NRST is a *system reset* that resets **all registers in the VCORE domain**, including the SWD/debug logic. This is different from some other ARM Cortex-M implementations where the debug port may survive system reset.

This means:
1. DP operations work while NRST is held low (confirmed by IDCODE reads)
2. When NRST is released, the DP state is cleared
3. A full SWD line reset + JTAG-to-SWD sequence is needed after NRST release

The code in [`swd_min::connect_under_reset_and_init()`](src/swd_min.cpp:485) implements an aggressive reconnect sequence after NRST release.

## Troubleshooting ACK=7 after NRST release

If you see ACK=7 (all ACK bits high = invalid/no response) on all operations after NRST release, the problem is likely one of:

1. **User firmware disables SWD pins extremely quickly** - Some firmware reconfigures PA13/PA14 (SWDIO/SWCLK) as GPIO within microseconds of reset. The only solution is to add a debug delay loop early in `Reset_Handler` or use a debug-strap GPIO.

2. **Hardware issue with NRST** - Verify with scope/meter that NRST actually rises cleanly to VDD when released. Check for shorts or pull-downs on the NRST line.

3. **Option bytes have SWD disabled** - If the device's option bytes disable debugging, SWD will not respond after reset. This requires "connect under reset" with power-cycling to recover.

4. **SWD pins have hardware conflicts** - Check that nothing else is driving PA13/PA14 (level shifters, other MCUs, strong pull-downs, etc.).

### Diagnostic steps:

1. **Try on a known-good board** (fresh chip or Nucleo) to verify the procedure works
2. **Measure NRST with scope** during the attach sequence
3. **Try without NRST control** - On STM32G0, NRST is not strictly required for SWD if the firmware cooperates
4. **If you can briefly connect**, immediately read option bytes and check debug settings
