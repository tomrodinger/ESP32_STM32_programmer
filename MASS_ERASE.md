# STM32G031 Mass Erase via SWD (Connect Under Reset)

This document describes how to perform a flash mass erase on the STM32G031 using "connect under reset", allowing recovery of devices where firmware has disabled SWD pins.

## Key Concept: What "Connect Under Reset" Really Means

**IMPORTANT CORRECTION**: "Connect under reset" does NOT mean holding NRST LOW while accessing peripheral registers!

When NRST is held LOW:
- The CPU is held in reset (cannot execute any code)
- The Debug Port (DP) remains functional
- The System Debug space (DEMCR, DHCSR) IS accessible
- **BUT peripheral registers (flash controller, GPIO, etc.) return their reset values (0x00000000)!**

This is because the AHB peripheral bus is held in reset state when NRST is asserted. The peripheral clocks are not running and register access doesn't work properly.

## The Correct Sequence Using VC_CORERESET

The proper "connect under reset" sequence uses the **VC_CORERESET** (Vector Catch on Core Reset) feature:

1. **Assert NRST LOW** (hold target in reset)
2. **Initialize Debug Port** via SWD (this works while NRST is LOW)
3. **Set VC_CORERESET bit in DEMCR** (0xE000EDFC bit 0) - this IS accessible while NRST is LOW because it's in the always-on debug system space
4. **Release NRST** - core exits reset but immediately HALTS before executing ANY instruction
5. **Peripherals are now accessible** and firmware has NOT executed (SWD pins not disabled)
6. **Perform flash operations**
7. **Clear VC_CORERESET** and clean up

## Flash Controller Register Addresses

**Source**: [`docs/stm32g031xx.h`](docs/stm32g031xx.h:604) (lines 248-267, 559-604, 2440-2523)

| Register | Offset | Address | Description |
|----------|--------|---------|-------------|
| FLASH_ACR | 0x00 | 0x40022000 | Access Control |
| FLASH_KEYR | 0x08 | 0x40022008 | Flash Key (unlock) |
| FLASH_OPTKEYR | 0x0C | 0x4002200C | Option Key |
| FLASH_SR | 0x10 | 0x40022010 | Status |
| FLASH_CR | 0x14 | 0x40022014 | Control |
| FLASH_OPTR | 0x20 | 0x40022020 | Option |
| FLASH_SECR | 0x80 | 0x40022080 | Security |

Base address calculation:
```
PERIPH_BASE     = 0x40000000
AHBPERIPH_BASE  = PERIPH_BASE + 0x00020000 = 0x40020000
FLASH_R_BASE    = AHBPERIPH_BASE + 0x00002000 = 0x40022000
```

## FLASH_SR (Status Register) Bit Definitions

**Source**: [`FLASH_SR_*`](docs/stm32g031xx.h:2440)

| Bit | Name | Description |
|-----|------|-------------|
| 16 | BSY1 | Busy flag (1 = operation in progress) |
| 18 | CFGBSY | Configuration busy flag |
| 15 | OPTVERR | Option validity error |
| 14 | RDERR | PCROP read error |
| 9 | FASTERR | Fast programming error |
| 8 | MISERR | Fast programming data miss error |
| 7 | PGSERR | Programming sequence error |
| 6 | SIZERR | Size error |
| 5 | PGAERR | Programming alignment error |
| 4 | WRPERR | Write protection error |
| 3 | PROGERR | Programming error |
| 1 | OPERR | Operation error |
| 0 | EOP | End of operation |

**IMPORTANT**: BSY1 is at **bit 16**, not bit 0!

## FLASH_CR (Control Register) Bit Definitions

**Source**: [`FLASH_CR_*`](docs/stm32g031xx.h:2481)

| Bit | Name | Value | Description |
|-----|------|-------|-------------|
| 0 | PG | 0x00000001 | Programming enable |
| 1 | PER | 0x00000002 | Page erase |
| 2 | MER1 | 0x00000004 | Mass erase (Bank 1) |
| 16 | STRT | 0x00010000 | Start operation |
| 17 | OPTSTRT | 0x00020000 | Option start |
| 31 | LOCK | 0x80000000 | Flash lock |
| 30 | OPTLOCK | 0x40000000 | Option lock |

## Flash Unlock Keys

**Source**: ST RM0444 Reference Manual (STM32G0x1)

```c
#define FLASH_KEY1  0x45670123UL
#define FLASH_KEY2  0xCDEF89ABUL
```

The flash must be unlocked by writing KEY1 then KEY2 to FLASH_KEYR in sequence.

## Complete Mass Erase Sequence

### Prerequisites
1. NRST held LOW (target in reset)
2. SWD communication established (line reset, IDCODE read OK)
3. Debug power domain enabled (CSYSPWRUPREQ + CDBGPWRUPREQ in DP CTRL/STAT)
4. AHB-AP selected and configured (CSW = 0x23000012 for 32-bit access)

### Step-by-Step Procedure

```
Step 1: Wait for any ongoing operation to complete
-------------------------------------------------
Read FLASH_SR (0x40022010)
Wait until BSY1 (bit 16) is clear

Step 2: Clear any error flags
-----------------------------
Write 0x0000C3FA to FLASH_SR (clears all error bits by writing 1s)

Step 3: Unlock the flash controller
-----------------------------------
Write 0x45670123 to FLASH_KEYR (0x40022008)
Write 0xCDEF89AB to FLASH_KEYR (0x40022008)
(These MUST be consecutive writes with no other flash operations between)

Step 4: Verify flash is unlocked
--------------------------------
Read FLASH_CR (0x40022014)
Check that LOCK bit (bit 31) is clear
If still locked, unlock failed - return error

Step 5: Set mass erase bit
--------------------------
Write 0x00000004 to FLASH_CR (sets MER1 bit)

Step 6: Start the erase
-----------------------
Write 0x00010004 to FLASH_CR (sets STRT bit while keeping MER1)

Step 7: Wait for completion
---------------------------
Poll FLASH_SR (0x40022010)
Wait until BSY1 (bit 16) is clear
Typical time: < 1 second for STM32G031 (up to 64KB flash)
Timeout: 10 seconds recommended

Step 8: Check for errors
------------------------
Read FLASH_SR (0x40022010)
Check error bits: OPERR, PROGERR, WRPERR, PGAERR, SIZERR, PGSERR
If any error bit is set, erase failed

Step 9: Clear MER1 bit
----------------------
Write 0x00000000 to FLASH_CR (clear operation bits)

Step 10: Lock the flash controller
----------------------------------
Write 0x80000000 to FLASH_CR (sets LOCK bit)
```

### Hex Summary

```
FLASH_KEYR = 0x40022008
FLASH_SR   = 0x40022010  
FLASH_CR   = 0x40022014

KEY1 = 0x45670123
KEY2 = 0xCDEF89AB

BSY1_MASK = 0x00010000 (bit 16)
MER1_MASK = 0x00000004 (bit 2)
STRT_MASK = 0x00010000 (bit 16)
LOCK_MASK = 0x80000000 (bit 31)

MER1 + STRT = 0x00010004
```

## SWD Transaction Sequence

Each flash register access requires these AHB-AP operations:

### Write to Flash Register
1. Set TAR (AHB-AP offset 0x04) to target address
2. Write data to DRW (AHB-AP offset 0x0C)

### Read from Flash Register  
1. Set TAR (AHB-AP offset 0x04) to target address
2. Read DRW (AHB-AP offset 0x0C) - returns previous read result
3. Read RDBUFF (DP offset 0x0C) - or read DRW again for actual data

## Implementation Notes

### NRST Handling
- **Keep NRST LOW throughout the entire mass erase operation**
- This prevents the CPU from executing any code that might interfere
- The flash controller operates independently of the CPU reset state
- After mass erase completes, releasing NRST will boot from empty flash

### Error Recovery
If mass erase fails:
1. Check RDP (Read Protection) level in FLASH_OPTR
2. If RDP is Level 1, changing to Level 0 triggers automatic mass erase
3. Ensure stable power supply during erase operation
4. Try reducing SWD clock speed

### BSY1 Timeout
- STM32G031 has max 64KB flash
- Mass erase typical time: 20-40ms per KB = ~2.5 seconds max
- Recommended timeout: 10 seconds
- If timeout occurs, flash may be in undefined state

### CSW Configuration
For AHB-AP memory access, CSW must be set to:
- Bits 2:0 = 0b010 (32-bit access size)
- Bits 5:4 = 0b01 (auto-increment)
- Bit 25 = 1 (privileged access)
- Bit 29 = 1 (master type)

Recommended CSW value: `0x23000012`

## References

1. STMicroelectronics RM0444 - STM32G0x1 Reference Manual
2. [`docs/stm32g031xx.h`](docs/stm32g031xx.h:1) - CMSIS Device Header
3. [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:1) - HAL Flash Driver
4. ARM Debug Interface v5 Architecture Specification (IHI0031)
