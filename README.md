# ESP32-S3 Programmer for STM32G031

This project demonstrates how to use an ESP32-S3 to program an STM32G031 microcontroller using the SWD (Serial Wire Debug) protocol. The ESP32-S3 acts as a standalone programmer, flashing a specific binary firmware to the target.

## Project Overview

-   **Programmer**: ESP32-S3 Development Board
-   **Target**: STM32G031 (ARM Cortex-M0+)
-   **Interface**: SWD (Serial Wire Debug)
-   **Firmware**: `bootloader_M17_hw1.5_scc3_1764326641.bin` (Embedded in ESP32 firmware)

## Hardware Connections

Connect the ESP32-S3 to the STM32G031 as follows:

| ESP32-S3 Pin | STM32G031 Pin | Description                   |
| :----------- | :------------ | :---------------------------- |
| **GND**      | **GND**       | Common Ground                 |
| **3V3**      | **VDD**       | Power (if ESP powers STM32)   |
| **GPIO 35**  | **SWCLK**     | Serial Wire Clock             |
| **GPIO 36**  | **SWDIO**     | Serial Wire Data Input/Output |
| **GPIO 37**  | **NRST**      | Reset Pin (held LOW during attach + IDCODE read) |

*Note: Pin assignments can be changed in [`src/swd_min.h`](src/swd_min.h:1) (see [`swd_min::Pins`](src/swd_min.h:13)).*

Available GPIOs on this board for SWD bit-banging: **35, 36, 37**.

## Software Implementation

The project is built using **PlatformIO** with the **Arduino** framework.

**Note on USB CDC**: The project is configured to use the ESP32-S3's native USB port for Serial communication (`USB_CDC_ON_BOOT=1`). This allows you to see the serial output directly via the USB connector used for programming.

### Key Components

1.  **`src/swd.cpp` / `src/swd.h`**:
    -   Implements the low-level SWD protocol (bit-banging).
    -   Handles SWD initialization, reset sequences, and DP/AP register read/write operations.

2.  **`src/stm32g0.cpp` / `src/stm32g0.h`**:
    -   Implements high-level STM32G0 specific flash operations.
    -   **Unlock**: Unlocks the flash memory using the STM32G0 key sequence.
    -   **Mass Erase**: Erases the entire flash memory.
    -   **Program**: Writes the firmware binary to flash address `0x08000000`.
    -   **Verify**: Reads back the flash content and compares it with the source binary.

3.  **`src/binary.h`**:
    -   Contains the target firmware (`bootloader_M17_hw1.5_scc3_1764326641.bin`) converted into a C byte array (`firmware_bin`).
    -   Generated using `convert_bin.py`.

4.  **`src/main.cpp`**:
    -   Provides a Serial interface (115200 baud).
    -   Waits for the user to send the character **'p'** to start the programming process.
    -   **Heartbeat**: The built-in LED (if available) blinks every 2 seconds, and "Waiting for 'p'..." is printed to Serial to indicate the board is running.

## Current status (as of this commit)

This repo has two parallel deliverables:

1. **ESP32-S3 firmware** that can program an STM32G031 over SWD.
2. A **macOS-hosted simulator + waveform viewer** that runs the *same bit-banged SWD host code* and visualizes the full programming sequence.

### Project goal (end state)

End-to-end programming should follow this sequence (both on hardware and in the simulator):

1. SWD attach + **DP `IDCODE` read** (prove physical/protocol layer)
2. **DP init + power-up** (ADIv5 `CTRL/STAT` handshake) via [`swd_min::dp_init_and_power_up()`](src/swd_min.cpp:392)
  3. **Connect + halt** core via [`stm32g0_prog::connect_and_halt()`](src/stm32g0_prog.cpp:76)
  4. **Flash mass erase** via [`stm32g0_prog::flash_mass_erase_under_reset()`](src/stm32g0_prog.cpp:275)
  5. **Flash program** (in the simulator: a small 8-byte payload) via [`stm32g0_prog::flash_program()`](src/stm32g0_prog.cpp:130)
  6. **Verify** by reading flash back and comparing via [`stm32g0_prog::flash_verify_and_dump()`](src/stm32g0_prog.cpp:186)

### What’s in this repo

- SWD host bit-bang + DP/AP + AHB memory access helpers: [`src/swd_min.cpp`](src/swd_min.cpp:1), [`src/swd_min.h`](src/swd_min.h:1)
- STM32G0 flash algorithms (unlock, mass erase, 64-bit programming, verify): [`src/stm32g0_prog.cpp`](src/stm32g0_prog.cpp:1), [`src/stm32g0_prog.h`](src/stm32g0_prog.h:1)
- ESP32 serial command harness (commands: `i/e/w/v/a`): [`src/main.cpp`](src/main.cpp:1)
- Simulator executable that runs the full flow and writes `signals.csv`: [`sim/main.cpp`](sim/main.cpp:1)
- Waveform viewer (Plotly HTML): [`viewer/view_log.py`](viewer/view_log.py:1)

### Hardware note (observed)

On real STM32G031 hardware, the target responded reliably only when **NRST is held LOW** during the initial SWD attach and IDCODE read (see [`swd_min::reset_and_switch_to_swd()`](src/swd_min.cpp:361)).

### SWDIO vs SWCLK edges (edge-only model for the simulator)

For the standalone signal simulator we simplify the SWD physical layer into **edge-triggered events only**:

| SWCLK edge | Host (probe) | Target (SWD-DP) |
|---|---|---|
| **Rising (↑)** | (no action; must be stable here) | **Sample SWDIO** (when host is driving) **and update SWDIO drive state** (when target is driving) |
| **Falling (↓)** | **Sample SWDIO** (when target is driving) **and update SWDIO drive state** (when host is driving) | (no action; must be stable here) |

This matches the practical interpretation that the **target is “rising-edge” (sample + change on ↑)**, therefore the **host must be “falling-edge” (change + sample on ↓)** to avoid races on a single bidirectional wire.

#### Turnaround expressed in edges (why it’s “½ bit” and “1½ bit”)

Because **the target can only start/stop driving on ↑** and **the host can only start/stop driving on ↓**, the “turnaround cycle” from the Arm packet diagrams becomes asymmetric in edge time:

- **Host → Target turnaround (request → ACK)**: **½-cycle of Z**
  - Host releases SWDIO on **↓** after the last request bit.
  - Target begins driving on the very next **↑** (ACK bit0 becomes valid for the following ↓ sample).

- **Target → Host turnaround (end of read / after ACK in write)**: **1½-cycles of Z**
  - Target releases SWDIO on **↑** after its last driven bit and the line floats.
  - Host does not take ownership until 1½ cycles of the clock later and then starts driving the line on a **↓**, so SWDIO stays `Z` across the next **↓** and **↑**.
  - Host begins driving on the following **↓**.

In this edge-only model, the only legal SWDIO transitions are:

- host drive changes on **↓**
- target drive changes on **↑**
- host samples on **↓**
- target samples on **↑**

## Usage

  1.  **Hardware Setup**: Wire the ESP32-S3 and STM32G031 according to the table above.
  2.  **Build & Upload**:
     -   Build the project: `pio run`
     -   Upload to ESP32-S3: `pio run -t upload`
  3.  **Run**:
     -   Open the Serial Monitor: `pio device monitor` (baud rate 115200).
     -   Reset the ESP32-S3.
     -   You should see:
        ```
        ESP32-S3 STM32G0 Programmer
        Firmware Size: 9220 bytes
        Send 'p' to start programming...
        ```
      -   Use serial commands:
          - `h` help
          - `i` reset + read DP IDCODE
          - `R` let firmware run: clear debug-halt state, pulse NRST (>=1ms low), then release SWD pins
          - `d` toggle SWD verbose diagnostics (prints DP/AP/memory access details)
          - `t` SWD smoke test (attempts DP power-up handshake + AHB-AP IDR read; may fail on current hardware)
          - `c` DP CTRL/STAT single-write test (writes DP[0x04]=0x50000000; requires `i` first)
          - `b` DP ABORT write test (writes DP[0x00]=0x1E under NRST low then high)
          - `r` read first 8 bytes of target flash @ `0x08000000`
          - `e` mass erase (connect-under-reset recovery method)
         - `w` write embedded firmware (prints a timing benchmark)
          - `v` verify embedded firmware
          - `a` all (connect+halt, erase, write, verify)

## Performance / benchmarking

The `w` command prints a simple on-device benchmark so you can track programming speed improvements.

- It temporarily disables SWD verbose logging (see [`swd_min::set_verbose()`](src/swd_min.cpp:14)), because Serial printing can dominate runtime.
- It reports:
  - connect time
  - program time
  - total time
  - a rough throughput estimate (KiB/s over the program phase)

Implementation details:

- Flash busy polling for programming uses microsecond-scale backoff instead of `delay(1)`.
- Bulk programming uses an AHB-AP “session” to avoid re-writing `SELECT/CSW/TAR` for every 32-bit access (major SWD traffic reduction).

## Known current limitations (bench)

- A DP **IDCODE read** appears to be required before subsequent DP writes will ACK reliably. This is why the workflow is typically:
  1. Run `i`
  2. Run other commands (`c`, `t`, `r`, etc.)

## Debug output behavior

- **SWD verbose is ON by default** at boot. Toggle it with `d`.
- Every non-whitespace keypress prints a banner like:
  - `=== User pressed r ========================`
- Whenever the firmware changes NRST state it prints a clear line like:
  - `---------------------------------------- NRST HIGH`
- The initial driven NRST state is printed at boot.

## Updating the Firmware

To change the firmware being flashed to the STM32:

1.  Place the new `.bin` file in the project root.
2.  Update `convert_bin.py` if the filename changes.
3.  Run the conversion script:
    ```bash
    python3 convert_bin.py
    ```
  4.  Rebuild and upload the ESP32 firmware.

## Automated build/upload + command runner (recommended)

For faster iteration, use the helper script:

1. Create and activate a local Python venv:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r tools/requirements.txt
```

2. Build + upload + run serial commands:

```bash
python3 tools/esp32_runner.py -i -r
```

Notes:

- Use `--port /dev/tty.usbmodemXXXX` if auto-detection chooses the wrong port.
- The script prints the device output and exits.
