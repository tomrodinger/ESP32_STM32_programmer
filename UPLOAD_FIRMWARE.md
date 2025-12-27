# Part 2 plan: program main firmware over RS485 (ESP32-S3 jig)

## My understanding of the project (current state)

1. This repo implements **Part 1 of 3**: programming an STM32 target over SWD from an ESP32-S3.
   - The target firmware image for Part 1 is stored in the ESP32 filesystem partition `fwfs` (mounted as SPIFFS at `/spiffs`). See [`cpp.firmware_fs::begin()`](src/firmware_fs.cpp:54).
   - File selection today is based on `BL*` naming (bootloader images) and a persisted selection file. See [`cpp.firmware_fs::get_active_firmware_path()`](src/firmware_fs.cpp:170).
   - The programming flow (`i/e/w/v/R` and production `<space>`) is implemented in [`src/main.cpp`](src/main.cpp:1).

2. During Part 1 programming (`w`), the jig assigns and injects **serial number + unique ID** into the target image before flashing.
   - The production flow already has `serial` and `unique_id` available in memory as `serial_log::Consumed consumed` within [`cpp.run_production_sequence()`](src/main.cpp:180).

3. Part 2 is to program a **second firmware file** (main firmware) using a **different physical interface and protocol**:
   - Transport: **RS485**
   - Protocol: **Servomotor new protocol only**, CRC32 enabled
   - Addressing: **targeted by unique ID** (not broadcast)
   - The unique ID to address is the same one we just programmed into the device during Part 1, so we must retain it through Part 2 (and later Part 3 tests).

4. You placed the main firmware file in the repo root:
   - `servomotor_M17_fw0.14.0.0_scc3_hw1.5.firmware`
   - You want it included in the filesystem image (the `fwfs` SPIFFS partition) and stored at the filesystem root (SPIFFS is effectively flat).

## Filename renaming scheme (for SPIFFS 31-char basename limit)

This repo already enforces a 31-character maximum firmware basename due to SPIFFS limits. See [`cpp.firmware_fs::k_max_firmware_basename_len`](src/firmware_fs.h:10).

Implementation note: filename normalization is implemented via one generic helper in its own module: [`cpp.filename_normalizer::normalize_basename()`](src/filename_normalizer.cpp:66). Bootloader and servomotor naming policies are thin wrappers around it.

For this **main firmware** file type, you specified a new renaming algorithm:

1. Take the basename (strip any directories).
2. If it starts with `servomotor`, replace that prefix with `SM`.
3. If it ends with `.firmware`, strip that suffix.
4. Ensure the final basename length is **<= 31** characters or less.

Applying that to the provided file:

- Input: `servomotor_M17_fw0.14.0.0_scc3_hw1.5.firmware`
- Output: `SM_M17_fw0.14.0.0_scc3_hw1.5` (28 characters)

### Policy for future longer filenames

For now, the cleanest deterministic behavior is:

- If the normalized basename is `> 31`, fail the staging step with a clear error (so we do not silently create ambiguous/duplicate names).

If you prefer an automatic truncation strategy (and how to avoid collisions), we can define it later; I will not guess without your direction.

## How the main firmware file gets into the ESP32 filesystem

The repo’s `fwfs` image is built from the host directory [`data/`](data/:1) using the custom targets defined in [`python.tools/pio_fwfs_targets.py`](tools/pio_fwfs_targets.py:1).

Plan for Part 2 assets:

1. Stage the renamed firmware file into [`data/`](data/:1) so it ships in the SPIFFS image:
   - `data/SM_M17_fw0.14.0.0_scc3_hw1.5`
2. Keep the existing `BL*` bootloader file approach unchanged.

## Servomotor RS485 protocol approach (matching your known-good Python behavior)

You indicated:

- ESP32 cannot run Python.
- We should implement the upgrade using your Arduino library.
- We should use **new protocol only** and **address by unique ID**.
- Direction control is handled by RS485 hardware (auto direction), so we should not need to manage DE/RE.

From the Arduino library:

- Command IDs include `FIRMWARE_UPGRADE = 23` and `SYSTEM_RESET = 27` in [`arduino.Commands`](../../Servomotor/Arduino_library/Commands.h:9).
- There is a high-level API to send a firmware page: [`cpp.Servomotor::firmwareUpgrade()`](../../Servomotor/Arduino_library/Servomotor.cpp:937).
- The underlying comms layer supports CRC32 and extended addressing by unique ID: [`cpp.Communication::sendCommandByUniqueId()`](../../Servomotor/Arduino_library/Communication.cpp:83).

### Firmware payload format for `FIRMWARE_UPGRADE`

The Arduino library defines the firmware page payload as 2058 bytes (`firmwarePage[2058]`) in [`arduino.firmwareUpgradePayload`](../../Servomotor/Arduino_library/Servomotor.h:142).

That matches the structure used in your Python process:

- `model_code` (8 bytes)
- `firmware_compatibility_code` (1 byte)
- `page_number` (1 byte)
- `page_data` (2048 bytes)

Total: `8 + 1 + 1 + 2048 = 2058`.

So the ESP32 implementation should:

1. Read the `.firmware` file from SPIFFS.
2. Parse out `model_code`, `firmware_compatibility_code`, and the remainder `firmware_data`.
3. Apply the same “prepare data before paging” transformations as your Python process (padding / CRC32 / size word rewrite), so the device’s bootloader receives exactly what it expects.
4. Iterate page-by-page, building each 2058-byte page payload and calling `firmwareUpgrade(unique_id, page)`.
5. Rate-limit page transmission if required (in Python you needed delays to avoid overflowing the device).
6. Send `systemReset(unique_id)` at the end to reboot into the new main firmware.

### Bootloader-entry timing

Your Python process resets into bootloader mode first, then sends pages quickly enough to beat the bootloader timeout.

On ESP32 we’ll mirror that by:

1. Ensuring the STM32 is running the bootloader (after Part 1 SWD flash, we already do an `R` reset pulse in production).
2. Immediately starting RS485 comms and sending `SYSTEM_RESET` (targeted by unique ID) to force bootloader entry.
3. Waiting a short fixed delay before the first page (same logic as Python).

## How Part 2 integrates into the jig state machine

### A) Keep a “unit context” in RAM for Part 1 → Part 2 → Part 3

Because Part 2 must address the device by **unique ID**, we need to preserve the ID assigned during Part 1.

Plan:

1. Introduce a single struct that holds per-unit context:
   - `serial_number`
   - `unique_id`
   - “valid” flag
   - potentially also: firmware filenames used (BL + SM)

2. Populate it at the same time we already “consume for write” in production.
3. Reuse it for Part 2 and later Part 3 tests.

Implementation detail: today the production flow already has `consumed.serial` and `consumed.unique_id` in scope in [`cpp.run_production_sequence()`](src/main.cpp:180). We’ll formalize this into a stored context so later steps don’t need to re-parse logs.

### B) Add a new command for RS485 upgrade

Add a new serial console command, for example:

- `u` = upgrade main firmware over RS485 using the staged `SM*` file and addressing by the most recently programmed `unique_id`.

Also extend the production `<space>` flow to include Part 2:

Current production: `i -> e -> w -> v -> R`

Proposed production (Part 1 + Part 2):

- `i -> e -> w -> v -> R -> u`

Rationale: `R` makes the target execute the freshly flashed bootloader, which is required to accept the RS485 firmware upgrade.

If you want the final step to reboot into main firmware after `u`, then `u` itself should end with a `SYSTEM_RESET` over RS485.

## Filesystem selection policy for the main firmware (SM*)

We should mirror the existing bootloader file selection policy:

- For Part 2, scan for exactly one file whose basename starts with `SM`.
- If exactly one exists, auto-select it.
- If multiple exist, refuse to program (deterministic production requirement).
- generalize the existing `firmware_fs` selection logic to accept a prefix like `BL` vs `SM`.

## Script requested: copy Arduino library files + stage firmware file

You requested a script that does both:

1. Copy the required Arduino library sources from your Servomotor repo into this repo.
2. Apply the renaming algorithm and stage the `.firmware` file into [`data/`](data/:1).

Plan:

- Add a host-side script under [`tools/`](tools/:1), e.g. `tools/stage_servomotor_assets.py`.
- The script will:
  1. Copy a curated subset of files from the Arduino library into a PlatformIO-friendly library directory, e.g. `lib/Servomotor/`.
     - Required core files (based on includes):
       - `Servomotor.cpp/.h`
       - `Communication.cpp/.h`
       - `Commands.h`
       - `DataTypes.cpp/.h`
       - `Utils.h`
       - `AutoGeneratedUnitConversions.cpp/.h`
     - Exclude tests, desktop emulator, and scripts.
  2. Normalize the firmware filename per the `servomotor` → `SM` rule and `.firmware` stripping.
  3. Copy `servomotor_M17_fw0.14.0.0_scc3_hw1.5.firmware` into `data/<normalized_name>`.
  4. Print exactly what it did (source paths, destination paths) and fail loudly on any mismatch.

Integrate this into [`build_and_upload.py`](build_and_upload.py:1) so a single command stages both BL and SM assets before `pio run -t buildfwfs`.

## Test and verification plan (implementation phase)

### Automated checks

1. `pio run` (warnings treated as failures)
2. `./test.sh` (existing repo automated checks)
3. Add unit tests for the new filename normalization logic, similar to [`cpp.test_firmware_name_utils`](testdata/test_firmware_name_utils.cpp:1).
4. Add a unit test for uploading the firmware via the 'u' command. This passes if every uploaded page is acked.

### Manual hardware test plan (required because this changes HW behavior)

1. Program a unit with the existing `<space>` flow and confirm Part 1 succeeds.
2. Run `u` and confirm the RS485 upgrade completes (no timeouts, no CRC errors).
3. Confirm the unit reboots into main firmware (e.g., via a known RS485 query such as firmware version).

Note: per [`AGENTS.md`](AGENTS.md:1), the implementation phase will include a short numbered manual test plan and will wait for your explicit confirmation before declaring completion.
