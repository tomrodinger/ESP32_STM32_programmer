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

*Note: Pin assignments can be changed in `src/swd.h`.*

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

The long-term goal remains **full chip programming over SWD** (mass erase + program + verify).

At the moment, we intentionally narrowed scope to first prove the physical/protocol layer by implementing an **as-simple-as-possible SWD bit-bang to read DP `IDCODE`**.

Confirmed on real STM32G031 hardware:
- DP IDCODE: `0x0BC11477`
- Critical detail: the target responded reliably only when **NRST is held LOW** during the initial SWD attach and IDCODE read.

What’s in the repo now:

- Minimal IDCODE-only SWD implementation: [`src/swd_min.cpp`](src/swd_min.cpp:1), [`src/swd_min.h`](src/swd_min.h:1)
- ESP32 serial test harness for IDCODE reads: [`src/main.cpp`](src/main.cpp:1)
- macOS simulator that compiles the same SWD code via an Arduino shim and produces a voltage log: [`sim/`](sim/:1)
- interactive waveform viewer (trackpad pan/zoom): [`viewer/view_log.py`](viewer/view_log.py:1)

Notes:

- The older “full programmer” code paths described below may not reflect the currently compiled firmware in [`src/main.cpp`](src/main.cpp:1). They are still the target end state.

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
    -   Type **`p`** and press Enter.
    -   The ESP32 will initialize SWD, erase the target, program the flash, and verify the content.
    -   Look for **"SUCCESS! Target programmed."**

## Updating the Firmware

To change the firmware being flashed to the STM32:

1.  Place the new `.bin` file in the project root.
2.  Update `convert_bin.py` if the filename changes.
3.  Run the conversion script:
    ```bash
    python3 convert_bin.py
    ```
4.  Rebuild and upload the ESP32 firmware.
