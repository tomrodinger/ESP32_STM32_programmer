# Plan: add Mode 2 `u` command to upgrade target firmware over RS485 (unique ID addressing)

This plan is derived directly from the known-good host upgrader script [`upgrade_firmware.py`](../../Servomotor/python_programs/upgrade_firmware.py:1) and from the Arduino Servomotor library vendored in this repo under [`lib/Servomotor/`](lib/Servomotor/:1).

## Goal

Add a new Mode 2 command:

- `u` = upgrade the motor controller main firmware over RS485.

Constraints you set:

- Firmware file is already stored on the ESP32 filesystem (partition `fwfs`).
- Device addressing must use the **unique ID**.
- With unique-ID addressing, **each packet must be ACKed**.
- If any page is not ACKed (timeout or other error), abort and print a clear error.

## Relevant existing code (what we will reuse)

Mode 2 already creates a `Servomotor motor` object and already proves RS485 comms via the `p` command:

- Mode 2 loop: [`src/mode2_loop.cpp`](src/mode2_loop.cpp:1)
- `p` implementation uses [`cpp.Servomotor::getProductInfo()`](lib/Servomotor/Servomotor.h:462)

Mode 2 already pulls the most-recently-programmed device ID and configures the motor for extended addressing:

- unit context: [`src/unit_context.h`](src/unit_context.h:10)
- read context: [`cpp.unit_context::get()`](src/unit_context.cpp:19)
- set unique ID: [`cpp.Servomotor::useUniqueId()`](lib/Servomotor/Servomotor.h:351)

So `u` must simply reuse the existing `motor` object.

## Firmware file discovery on ESP32

The filesystem partition is mounted as SPIFFS, using label `fwfs`:

- mount: [`cpp.firmware_fs::begin()`](src/firmware_fs.cpp:54)

Selection rule (deterministic):

1. List files in filesystem root that start with `SM` using [`cpp.firmware_fs::list_servomotor_firmware_basenames()`](src/firmware_fs.cpp:112).
2. If **exactly one** is found, use `/<basename>` as the firmware path.
3. If **zero** are found: error and abort.
4. If **multiple** are found: error listing candidates and abort.

## Firmware file format (from the Python upgrader)

From [`read_binary()`](../../Servomotor/python_programs/upgrade_firmware.py:110), the file structure is:

- `model_code`: 8 bytes
- `firmware_compatibility_code`: 1 byte
- `firmware_data`: remaining bytes

The on-wire payload for the firmware upgrade command is (also matches [`arduino.firmwareUpgradePayload`](lib/Servomotor/Servomotor.h:142)):

- `model_code` (8)
- `firmware_compatibility_code` (1)
- `page_number` (1)
- `page_data` (2048)

Total = 2058 bytes.

## Required transformation of firmware_data before paging (critical)

From the host upgrader main flow (padding, size word rewrite, CRC append):

- padding: [`upgrade_firmware.py`](../../Servomotor/python_programs/upgrade_firmware.py:404)
- size+CRC calculation: [`upgrade_firmware.py`](../../Servomotor/python_programs/upgrade_firmware.py:419)
- rewrite/append: [`upgrade_firmware.py`](../../Servomotor/python_programs/upgrade_firmware.py:430)

Algorithm to mirror exactly:

1. Pad `firmware_data` with `0x00` until `len(firmware_data) % 4 == 0`.
2. Compute:
   - `firmware_size_words = (len(firmware_data) / 4) - 1`
   - `firmware_crc32 = crc32(firmware_data[4:])`
3. Build transformed payload:
   - `tx = firmware_size_words (4 bytes, little endian) + firmware_data[4:] + firmware_crc32 (4 bytes, little endian)`

CRC32 implementation to use (already in this repo):

- [`cpp.calculate_crc32()`](lib/Servomotor/Communication.h:53) implemented in [`cpp.calculate_crc32()`](lib/Servomotor/Communication.cpp:39)

## Paging rules

Constants from the host upgrader:

- page size: 2048 bytes ([`FLASH_PAGE_SIZE`](../../Servomotor/python_programs/upgrade_firmware.py:62))
- bootloader pages: 5 ([`BOOTLOADER_N_PAGES`](../../Servomotor/python_programs/upgrade_firmware.py:63))
- first firmware page number: 5 ([`FIRST_FIRMWARE_PAGE_NUMBER`](../../Servomotor/python_programs/upgrade_firmware.py:64))
- last firmware page number: 30 ([`LAST_FIRMWARE_PAGE_NUMBER`](../../Servomotor/python_programs/upgrade_firmware.py:65))

Send `tx` in 2048-byte pages starting at page number 5.

For each page:

1. Copy 2048 bytes from `tx` (pad final page with 0x00 to exactly 2048).
2. Construct `firmwarePage[2058]` = header (10 bytes) + page_data (2048).
3. Send it and wait for ACK.
4. If `page_number > 30`, abort with error.

## Reset into bootloader + timing

Host upgrader resets into bootloader then waits 70ms:

- reset: [`upgrade_firmware_new_protocol()`](../../Servomotor/python_programs/upgrade_firmware.py:295)
- wait: [`WAIT_FOR_RESET_TIME`](../../Servomotor/python_programs/upgrade_firmware.py:68)

ESP32 flow (current jig behavior):

- The Mode 2 `u` command does **not** send any reset commands.
- The operator must ensure the target is already in the bootloader (for example: run Mode 1 `R`, then press `2` to enter Mode 2).
- Then `u` begins sending firmware pages immediately.

## ACK / error handling

With unique ID addressing, every firmware page must be acknowledged.

Implementation check after each page:

- use [`cpp.Servomotor::getError()`](lib/Servomotor/Servomotor.h:356)
- timeout code is [`COMMUNICATION_ERROR_TIMEOUT`](lib/Servomotor/Communication.h:11)

Rule:

- If `motor.getError() != 0` after sending a page, abort immediately and print:
  - page number
  - error code
  - if timeout, explicitly print `timeout`

Important: do not implement any “broadcast/no-ACK fallback” for `u`.

## Chunking vs delays (to prevent dropped bytes)

The host upgrader sends each large firmware packet in ~1000-byte chunks with a short delay:

- chunk loop: [`program_one_page()`](../../Servomotor/python_programs/upgrade_firmware.py:218)

In this repo we will **not modify** the vendored Arduino library implementation.

Mitigation we can do strictly in the jig application code (without changing the library):

- Add an optional fixed delay *between pages* if needed (still requiring ACK per page), similar in spirit to [`DELAY_AFTER_EACH_PAGE`](../../Servomotor/python_programs/upgrade_firmware.py:69) but only used if observed necessary.

If inter-page delay is not sufficient and we still see dropped bytes, the deficiency would be that the library transmits a very large frame without pacing, while the known-good host script demonstrates that pacing can be required. In that case, we would need your approval to change the library.

The Arduino library currently writes payloads in a single call inside [`cpp.Communication::sendCommandCore()`](lib/Servomotor/Communication.cpp:87). 

## Mode 2 `u` command: end-to-end flow

1. Confirm unit context unique ID exists (non-zero). See [`cpp.unit_context::get()`](src/unit_context.cpp:19).
2. Mount firmware filesystem via [`cpp.firmware_fs::begin()`](src/firmware_fs.cpp:54).
3. Locate firmware file `/<SM...>` via [`cpp.firmware_fs::list_servomotor_firmware_basenames()`](src/firmware_fs.cpp:112).
4. Open and read file header + data.
5. Transform firmware_data into `tx` exactly as described above.
6. For pages starting at page 5:
   - call [`cpp.Servomotor::firmwareUpgrade()`](lib/Servomotor/Servomotor.h:465)
   - check ACK via [`cpp.Servomotor::getError()`](lib/Servomotor/Servomotor.h:356)
   - abort on any error
7. After last page, do **not** reset from the jig. Reboot strategy is an operator/system decision.
8. Post-check (out of scope for this task): query firmware version using [`cpp.Servomotor::getFirmwareVersion()`](lib/Servomotor/Servomotor.h:471).

---

## Current status (handoff to next AI)

### What is implemented and working

1. Mode 2 command `u` exists and attempts to send the firmware upgrade stream over RS485:
   - command dispatch: [`src/mode2_loop.cpp`](src/mode2_loop.cpp:1)
   - implementation: [`cpp.servomotor_upgrade::upgrade_main_firmware_by_unique_id()`](src/servomotor_upgrade.cpp:63)

2. Firmware file discovery on ESP32 works:
   - it auto-selects exactly one `SM*` file from SPIFFS root via [`cpp.firmware_fs::list_servomotor_firmware_basenames()`](src/firmware_fs.cpp:112)
   - it reads `model_code` (8 bytes) + `firmware_compatibility_code` (1 byte) from the file header.

3. Data transformation matches the authoritative host upgrader:
   - pad firmware data to 4-byte multiple
   - compute `firmware_size_words = (len(data)/4) - 1`
   - compute `crc32(data[4:])`
   - build `tx = size_words(LE32) + data[4:] + crc32(LE32)`
   - then page `tx` into 2048-byte chunks, each sent with the required per-page header (model+compat+page#).
   Reference script: [`upgrade_firmware.py`](../../Servomotor/python_programs/upgrade_firmware.py:404)

4. Mode switching / SWD pin handling was adjusted so RS485 can work reliably:
   - entering Mode 2 floats SWD-related pins via [`cpp.swd_min::release_swd_and_nrst_pins()`](src/swd_min.cpp:611), which
   seems to be necessary to be able to software reset the device and
   keep it in a working state.
   - returning to Mode 1 restores SWD pins via [`cpp.swd_min::begin()`](src/swd_min.cpp:588)
   Implementation glue: [`src/main.cpp`](src/main.cpp:85)

### What is still failing

- The *first* firmware page transmission (page 5) does not get ACKed (timeout) when sent from the ESP32 implementation, while the Python upgrader can perform the same operation successfully on the same hardware (firmware being sent from a Mac computer via the upgrade_firmware.py program).
- This strongly suggests a mismatch between the Python sender behavior and the ESP32/Arduino-library sender behavior for the very large firmware-upgrade frames.

### Most likely root cause candidates (do not assume; investigate)

1. **TX pacing / UART buffering differences**
   - The Python upgrader sends large packets in chunks with delays:
     - see chunk loop in [`program_one_page()`](../../Servomotor/python_programs/upgrade_firmware.py:218)
   - The Arduino library currently transmits the full payload via a single `_serial.write(payload, payloadSize)` call in [`cpp.Communication::sendCommandCore()`](lib/Servomotor/Communication.cpp:176)
   - If the RS485/serial path drops bytes without pacing, the device will discard the packet (CRC mismatch / framing issue) and therefore not ACK.

3. **Protocol mismatch on the wire**
   - Less likely, because the same library *does* work for smaller commands (Mode 2 `p`).
   - Still, capture the raw bytes of the first firmware-upgrade packet from ESP32 and compare to Python’s packet (size encoding + CRC32) to eliminate this.

### Constraints / decisions made

- The vendored Arduino library under [`lib/Servomotor/`](lib/Servomotor/:1) must not be modified without explicit approval.
- `u` was changed to **not** send RS485 `SYSTEM_RESET` before/after the upgrade; the operator is expected to place the target into bootloader mode separately.

### Recommended next debug steps (for next AI)

1. Add diagnostic instrumentation *without changing the protocol*:
   - log the first page packet length and optionally CRC computed over the outgoing bytes. This already happens in verbose mode.

2. Compare ESP32 packet vs Python packet:
   - ensure the same size-byte encoding rules are used and CRC32 covers the same byte sequence.

3. If packet bytes match, focus on pacing/timeout:
   - try increasing the inter-page delay and/or adding a pre-delay before sending page 5 (application-level, still requiring ACK)
   - if still failing, seek approval to patch the Arduino library to implement chunked transmission for large payloads and/or increase timeout.

## Implementation locations (next mode)

- Add `u` handling to the switch in [`src/mode2_loop.cpp`](src/mode2_loop.cpp:105).
- Implement the file reading + paging logic in the existing module [`src/servomotor_upgrade.cpp`](src/servomotor_upgrade.cpp:1) and declare a clean API in [`src/servomotor_upgrade.h`](src/servomotor_upgrade.h:1).

## Manual hardware test plan (required)

1. Program a unit in Mode 1 so that `unit_context` contains a valid unique ID.
2. Switch to Mode 2.
3. Run `p` and confirm product info is readable.
4. Run `u` and confirm:
   - each page is ACKed (no timeouts)
   - upgrade completes
   - device resets into new firmware
5. Run a version/info query after upgrade to confirm the new firmware is running. This is out of scope of this task but is definitly slated to be done in a following task.
