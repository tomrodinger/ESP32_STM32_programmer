# WiFi AP + Web UI + Serial Log (ESP32-S3)

## Understanding of this repo

- ESP32-S3 (Arduino/PlatformIO) acts as a standalone SWD programmer for STM32G031.
- The target firmware binary is stored in ESP32 SPIFFS (partition label `fwfs`) and selected by scanning for exactly one `BL*` file via [`firmware_fs::find_single_firmware_bin()`](src/firmware_fs.cpp:62).
- Production programming is a fail-fast sequence `i -> e -> w -> v -> R` triggered by Serial `<space>` or the GPIO45 jig button via [`run_production_sequence()`](src/main.cpp:134).

## Feature goal

1) ESP32-S3 exposes a **WiFi Access Point** + **simple web page** (browser UI) to:
- show the **next serial number** to program (uint32)
- allow setting/changing it
- show the **firmware filename** that will be written

2) WiFi + web server run on the **other core** to avoid impacting the factory programming loop.

3) Serial number persistence moves to an **append-only text log** stored in SPIFFS; after boot we derive the next serial from the log.

4) Serial consumption rule: after `e` succeeds (just before `w`), consume a serial number. `w` uses the consumed serial.

## Implementation status (this repo)

- Serial log is implemented at [`serial_log::begin()`](src/serial_log.cpp:73) and stored at `/log.txt` on the `fwfs` SPIFFS partition.
- Web UI is implemented in [`wifi_web_ui::start_task()`](src/wifi_web_ui.cpp:138) (WiFi AP + HTTP server), pinned to core 0.
- Product-info injection is implemented in [`firmware_source::ProductInfoInjectorReader`](src/product_info_injector_reader.h:1):
  - Reads the first 256-byte block, patches `serial_number` + `unique_id` inside the `product_info_struct`, then serves patched bytes.
  - All remaining bytes are pass-through from SPIFFS.
- Production sequence is updated in [`run_production_sequence()`](src/main.cpp:126): `i -> e -> (consume serial) -> w -> v -> R`.

### Secrets (not committed)

- Copy [`include/wifi_secrets.example.h`](include/wifi_secrets.example.h:1) to `include/wifi_secrets.h` and edit SSID/password.
- `include/wifi_secrets.h` is ignored by git via [`.gitignore`](.gitignore:1).

### Boot policy / default serial

- If `/log.txt` is missing/empty (no newline-terminated, parsable lines), `serial_next` is **NOT SET**.
- Production programming is disabled until you set `serial_next` via the web UI (`POST /api/serial`).

## Extra commands added (for factory visibility)

- `s`: re-scan `/log.txt`, print last valid serial, and load `serial_next`.
  - Policy: if last valid line is `USERSET_<N>`, then `serial_next=N`.
  - Otherwise `serial_next=last_serial+1`.
  - Runs automatically at boot via [`cmd_sync_serial_from_log()`](src/main.cpp:654).
- `l`: print the full `/log.txt` to Serial.
- `w`: prints the reserved `serial` + generated `unique_id`, dumps the first 256 bytes (post-injection) in hex, and prints the decoded `product_info_struct`.
- `S<serial>`: append `USERSET_<serial>` and immediately re-sync/print the derived `serial_next`.

## Web UI additions

- Added `GET /api/log` and a "Download Log" button that fetches and displays the full log in a scrollable window.

## Notes / deviations from the original plan section below

- The sections under **Plan (implementation-ready)** are kept for historical context.
- Current behavior is implemented in code and may differ from the earlier plan text:
  - Verify uses a runtime snapshot of the injected first 256 bytes (captured during the most recent `w` / production `w`) to avoid verify mismatches caused by serial injection.
    See [`firmware_source::FirstBlockOverrideReader`](src/first_block_override_reader.h:1).
  - For manual testing, `w` reserves a serial immediately and appends `w_<serial>` to `/log.txt`.

## New inputs / constraints (from factory tooling)

- The authoritative product-info layout is in [`include/product_info.h`](include/product_info.h:1):
  - `PRODUCT_INFO_MEMORY_LOCATION = 0x8000010` (absolute STM32 address)
  - `struct product_info_struct` is `__packed__` and contains `model_code[8]`, `firmware_compatibility_code`,
    `hardware_version_{bugfix,minor,major}`, `serial_number`, `unique_id`, `not_used`.
- The Mac tool writes that struct into the image at offset `PRODUCT_INFO_MEMORY_LOCATION - 0x08000000` in
  [`save_bin_file()`](generate_product_info.c:155).

Practical note:
- Because `PRODUCT_INFO_MEMORY_LOCATION = 0x08000010`, the patch offset is `0x10` from flash base, so the
  entire `product_info_struct` is inside the first 256 bytes. This matches the “only patch the first 256-byte block”
  simplicity constraint.

### Simplicity requirement for the patching reader

- Keep the patching code very simple for review:
  - read block-by-block
  - use a small fixed block size (e.g. 256 bytes)
  - only the first block is ever modified
  - apply the modification once after the full first block is read
  - remaining blocks are pass-through bytes from SPIFFS

## Plan (implementation-ready)

### A) WiFi AP + web server pinned to the other core

- Add a FreeRTOS task pinned to the other core (e.g. via `xTaskCreatePinnedToCore`) that:
  - starts WiFi AP (SSID/pass from a local secret header)
  - starts a tiny HTTP server
  - serves a single HTML page + 2 API endpoints

Endpoints:
- `GET /` -> static HTML (embedded string)
- `GET /api/status` -> JSON: `{ firmware_filename, serial_next }`
- `POST /api/serial` -> accepts `{ serial_next }`, persists it, returns updated status

UI behavior:
- Page polls `/api/status` every few seconds.
- User can set next serial; after set, it updates immediately.
- The UI shows a "Current status" JSON block (e.g. `{ "firmware_filename":"/BL_...", "serial_next":500 }`) just above the "Download Log" button.

### B) Secrets (not committed)

- Add a local secret header ignored by git:
  - `include/wifi_secrets.h` (or another location you choose; see “Open decisions” below)
- Commit `include/wifi_secrets.example.h`.
- Update [`.gitignore`](.gitignore) to ignore the real secret.

### C) Serial number persistence: append-only SPIFFS log

**Log file path** (proposal): `/log.txt` on the same SPIFFS partition used by firmware.

**Line formats** (append-only, text, short):

1) Serial reserved/consumed after `e` succeeds:

`e_<serial>\n`

2) Full success marker at end of production flow:

`ewvR_<serial>_OK\n`

In case of failure of any step we append _FAIL:

`ew_<serial>_FAIL\n`

Only te steps that passed will be logged. So, if a fail happens then we expect not all the steps to show and we will know what step failed (the step that follows that was not logged)

Example (as requested):

`iewvR_293875_OK\n` (no carriage return; not Windows)

**Write policy** (to minimize corruption):
- Always open with append, write exactly one line, `flush()`, close.
- Never rewrite in-place.

Additional robustness rules (so torn writes are survivable):
- When parsing, only consider a line “valid” if it ends with `\n`.
- Ignore a final trailing partial line (no newline).
- Keep each line short (single line < 64 bytes) so the append write is less likely to tear.

**Boot policy** (derive next serial):
- Read the log and locate the last **valid** line (newline-terminated, parses, optional CRC matches).
- Compute:
  - if last valid line consumed serial `S`, then `serial_next = S + 2` (skip one serial at boot to cover the case where a serial was consumed but logging failed).
- If log missing/empty: use a default seed (see “Open decisions”).

**Setting next serial via web UI** (no rewrite):
- Append a “set” line, e.g. `USERSET_<serial>\n`.
- Boot parser rule: we use the last serial number in the log whether it is in a `USERSET_` line or a normal programming line that ends with _OK or _FAIL.

### D) Integrate with production flow (consume after `e`, use in `w`)

Modify [`run_production_sequence()`](src/main.cpp:119) behavior:

1) Before `e`: read current `serial_next` from the parsed log state.
2) After [`cmd_erase()`](src/main.cpp:169) succeeds:
   - `consumed_serial = serial_next`
   - append `e_<consumed_serial>` to the log (open-append-close)
   - keep `consumed_serial` in RAM for this run
3) In `w`: inject/use `consumed_serial` when creating the programmed image (details below).
4) After verify + reset succeed: append `ewvR_<consumed_serial>_OK`.

If `w/v/R` fail after consumption, we do **not** append the OK line. On next boot, the `+2` rule prevents reusing the consumed serial.

Log format alignment note:
- The example line includes `i` (`iewvR_...`). The current production sequence in [`run_production_sequence()`](src/main.cpp:119)
  is `e -> w -> v -> R` and does not explicitly run the `i` command. We need to add 'i' to the production sequence. It will be the main way to know if the target is connected.

### E) Serial number injection + product info (emulate Mac tool)

Your Mac tool patches a `product_info_struct` into the binary before programming:
- It writes the struct into the image at offset `PRODUCT_INFO_MEMORY_LOCATION - 0x08000000` via `memcpy(...)` in [`save_bin_file()`](generate_product_info.c:155).

Plan on ESP32-S3:

1) Bring `product_info` definitions into this repo (must be authoritative):
   - `PRODUCT_INFO_MEMORY_LOCATION`
   - `struct product_info_struct` layout
2) Implement a firmware reader adapter that wraps the SPIFFS file reader:
   - Pass-through all bytes except for the product info region, where it substitutes the struct bytes.
   - This allows using the existing streaming path [`stm32g0_prog::flash_program_reader()`](src/stm32g0_prog.h:47) without buffering the full file.
3) Populate fields (now or incremental):
   - `serial_number = consumed_serial`
   - `unique_id`: ESP32-generated (non-deterministic random numner). Can be cryptographically safe or not, but must not repeat if rebooted. Need to be careful about the seed if using a pseudorandom number generator, or can use a hardware based random number generator if available.
   - model/hw/compat codes: assume they are already correctly present in the firmware image; we only inject `serial_number` + `unique_id` into the existing `product_info_struct` bytes

## Open decisions (need input)

1) Where should the local WiFi secret live?
   - Proposal: `include/wifi_secrets.h` (ignored) + `include/wifi_secrets.example.h` (committed)

2) What is the default serial seed if `/log.txt` is missing/empty?
A: programming must be disabled. User needs to log into the web interface and set it.

3) `product_info.h` source
Resolved: it is now present in the repo as [`include/product_info.h`](include/product_info.h:1).

## Extra info that will likely be needed during implementation

- Confirm whether we should log failures as a single summary line like `ew_<serial>_FAIL\n` (as written above), or whether
  you prefer always logging the full attempted step string (e.g. `ewv_<serial>_FAIL\n` if it failed at `R`).
A: the latter is correct. we need to include the letters of the steps that were attempted so we can easily see what step failed.
- Confirm whether it is acceptable that consuming the serial happens right after `e` success (per requirement), so if later
  steps fail there will be a “gap” (and boot logic also adds an additional `+1` gap).
