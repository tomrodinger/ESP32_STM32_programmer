<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# The Engineer's Guide to SWD for STM32

This guide breaks down the Serial Wire Debug (SWD) protocol into the functional layers you need to build your own ESP32-based programmer.

## 1. The Physical Layer (Hardware)

At its core, SWD is a synchronous, bidirectional serial protocol. Unlike UART, it uses a shared data line, and unlike SPI, it is half-duplex on that single line.

* **SWCLK (Clock)**: Always driven by the Host (your ESP32).
* **SWDIO (Data)**: Bidirectional. Driven by the Host to send commands and data; driven by the Target (STM32) to send responses and data.
* **GND**: Common ground is critical for signal integrity.

**Voltage Note**: STM32 chips typically operate at 3.3V. Ensure your ESP32 is also using 3.3V logic (which it does natively).

## 2. The Protocol Layer (The Conversation)

SWD is transaction-based. Every interaction follows a strict "Command -> Response -> Data" structure.

### The Basic Packet Structure

Every operation starts with the Host sending an 8-bit request header.


| Bit | Name | Description |
| :-- | :-- | :-- |
| 1 | **Start** | Always `1`. |
| 2 | **APnDP** | `0` = Access Debug Port (Configuration), `1` = Access Access Port (Memory/Peripherals). |
| 3 | **RnW** | `0` = Write, `1` = Read. |
| 4-5 | **Addr** | A[3:2] (Address bits) to select one of 4 registers in the current bank. |
| 6 | **Parity** | Odd parity for the bits 2–5. |
| 7 | **Stop** | Always `0`. |
| 8 | **Park** | Always `1`. |

### The Turnaround (Trn)

Since SWDIO is shared, the line must "turn around" when control switches between Host and Target. This is a period of **1 clock cycle** where neither side drives the line (it floats high).

### The Sequence

1. **Host Sends Header** (8 bits)
2. **Turnaround** (1 cycle) – *Host stops driving, Target takes over.*
3. **Target Sends ACK** (3 bits):
    * `OK` (001): Ready to proceed.
    * `WAIT` (010): Target is busy, try again later.
    * `FAULT` (100): Error occurred (e.g., stuck bit).
4. **Transaction Continues (Depends on command)**:
    * **If Write**: Turnaround (1 cycle) -> Host writes Data (32 bits + Parity).
    * **If Read**: Target writes Data (32 bits + Parity) -> Turnaround (1 cycle).

## 3. The STM32 Connection Sequence

You cannot just start reading memory. You must perform a specific "handshake" to wake up the chip and switch it from JTAG mode (default) to SWD mode.

1. **Line Reset**: Drive SWDIO High for 50+ clock cycles. This resets the internal state machine.
2. **JTAG-to-SWD Switch**: Send the specific 16-bit sequence `0xE79E` (LSB first). This tells the CoreSight hub to switch protocols.
3. **Line Reset (Again)**: Drive SWDIO High for 50+ clock cycles.
4. **Read IDCODE**: Read the `IDCODE` register (Address `0x00` in DP) to confirm connection. You should see a value like `0x1BA01477` (STM32F1) or similar.

## 4. Accessing Memory (The "Mission")

To program the flash, you need to write to the STM32's memory-mapped registers. You don't do this directly; you do it through the **MEM-AP** (Memory Access Port).

### The mechanism

1. **Select the MEM-AP**: Write to the DP's `SELECT` register to choose the Memory Access Port (usually AP \#0).
2. **Set the Address**: Write the target memory address (e.g., `0x08000000` for Flash start) to the **TAR** (Transfer Address Register) in the MEM-AP.
3. **Read/Write Data**: Read or Write the **DRW** (Data Read/Write) register in the MEM-AP. The debug interface automatically forwards this value to/from the address in TAR.

## 5. Flash Programming Sequence (STM32 Specifics)

Programming STM32 flash requires unlocking a specific controller and following a sequence. All these steps are performed by writing to memory addresses using the MEM-AP method described above.

### A. Unlock the Flash Controller

The Flash Control Register (`FLASH_CR`) is locked after reset. You must write two "Key" values to the Flash Key Register (`FLASH_KEYR`).

* **Key 1**: `0x45670123`
* **Key 2**: `0xCDEF89AB`


### B. Erase Flash (Page Erase)

1. Check `FLASH_SR` (Status Register) to ensure the **BSY** (Busy) bit is 0.
2. Set the **PER** (Page Erase) bit in `FLASH_CR`.
3. Write the page address you want to erase to `FLASH_AR`.
4. Set the **STRT** (Start) bit in `FLASH_CR`.
5. Wait until **BSY** bit clears.

### C. Write Data

1. Check `FLASH_SR` to ensure **BSY** is 0.
2. Set the **PG** (Programming) bit in `FLASH_CR`.
3. Write your data (16-bit or 32-bit depending on family) to the target address.
4. Wait until **BSY** bit clears.

*Note: Register addresses vary by family. For STM32F1 (Blue Pill), Flash base is `0x40022000`. For STM32F4, it is `0x40023C00`.*

## 6. Developing the ESP32 Programmer

For your jig, you have two main paths:

### Path A: Use an Existing Library (Recommended)

Don't reinvent the wheel. Several open-source projects have ported the low-level signal driving to ESP32.

* **ESP32_BlackMagic**: A port of the famous "Black Magic Probe" firmware to ESP32. It exposes a GDB server over WiFi, allowing you to flash directly.
* **DAPLink / CMSIS-DAP**: There are ESP32 implementations of the CMSIS-DAP protocol. This allows the ESP32 to act as a standard USB debugger (if using S2/S3) or a WiFi bridge.


### Path B: "Bit-Banging" (Custom Implementation)

If you need a simple, standalone "press button to flash" jig without a PC connection:

1. **Wiring**: Connect specific GPIOs to SWCLK and SWDIO.
2. **Timing**: SWD is robust. You can clock it slowly (e.g., 100kHz - 1MHz). Even `digitalWrite` on ESP32 is fast enough for basic functionality, though direct register manipulation (GPIO bit-set/bit-clear) is cleaner.
3. **Code Structure**:

```c
void sendHeader(uint8_t request) {
   // Loop 8 bits
   // set SWDIO based on bit
   // pulse SWCLK (High -> Low)
}

void turnAround() {
   pinMode(SWDIO_PIN, INPUT); // Float
   pulseClock();
}
```

4. **Flash Algorithm**: Store your binary in the ESP32's flash (SPIFFS/LittleFS). Read it in chunks, and loop through the **Write Data** sequence (step 5C) for every word.

### Critical Tip: Handling Parity \& Errors

The `ACK` response is your only feedback.

* If you receive **WAIT**: Retry the same transaction.
* If you receive **FAULT**: You likely need to clear the sticky error bits in the DP `CTRL/STAT` register (`ABORT` register).
* If you receive **Garbage**: Check your signal wires and ground. SWD is sensitive to loose connections.
<span style="display:none">[^1][^10][^11][^12][^13][^14][^15][^16][^17][^18][^19][^2][^20][^21][^22][^23][^24][^25][^26][^27][^28][^3][^4][^5][^6][^7][^8][^9]</span>

<div align="center">⁂</div>

[^1]: https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/imxrt/4786/2/Serial_Wire_Debug.pdf

[^2]: https://www.silabs.com/documents/public/application-notes/an0062.pdf

[^3]: https://openocd.org/doc/html/Debug-Adapter-Configuration.html

[^4]: https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/serial-wire-debug-port-interface-for-nrf52832

[^5]: http://qcentlabs.com/posts/swd_banger/

[^6]: https://www.st.com/resource/en/programming_manual/pm0063-stm32f100xx-value-line-flash-programming-stmicroelectronics.pdf

[^7]: https://www.pcbway.com/blog/technology/OpenOCD_on_Raspberry_Pi__Better_with_SWD_on_SPI.html

[^8]: https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/lpc/55224/1/SWD Programming AN11553.pdf

[^9]: https://documentation-service.arm.com/static/5f900a19f86e16515cdc041e?token=

[^10]: https://www.st.com/resource/en/programming_manual/pm0059-stm32f205215-stm32f207217-flash-programming-manual-stmicroelectronics.pdf

[^11]: https://forums.swift.org/t/help-using-the-tft-espi-arduino-library-on-esp32-with-swift/78649

[^12]: https://markding.github.io/swd_programing_sram/

[^13]: https://nebkelectronics.wordpress.com/2016/10/15/writing-to-stm32-flash/

[^14]: https://www.walduk.at/2024/11/28/understanding-stm32s-flash-protection/

[^15]: https://community.st.com/t5/stm32-mcus-boards-and-hardware/jtag-swd-nucleo-over-spi-code-example/td-p/604994

[^16]: https://ioprog.com/2018/05/23/using-flash-memory-on-the-stm32f103/

[^17]: https://stackoverflow.com/questions/63125835/why-doesnt-my-flash-control-register-update-when-i-write-to-it-stm32

[^18]: https://forum.arduino.cc/t/how-to-use-the-github-custom-library/1360101

[^19]: https://docs.platformio.org/en/latest/plus/debug-tools/cmsis-dap.html

[^20]: https://docs.platformio.org/en/stable/plus/debug-tools/blackmagic.html

[^21]: https://www.reddit.com/r/esp32/comments/yxpl4a/standalone_executable_file_for_windows_command/

[^22]: https://learn.adafruit.com/circuitpython-with-esp32-quick-start/overview

[^23]: https://github.com/windowsair/wireless-esp8266-dap

[^24]: https://www.reddit.com/r/esp32/comments/13u8wr5/use_esp32_as_jtagswd_wireless_debugger_probe/

[^25]: https://github.com/atc1441/ESP32_nRF52_SWD

[^26]: https://randomnerdtutorials.com/getting-started-with-esp32/

[^27]: https://github.com/bkuschak/cmsis_dap_tcp_esp32

[^28]: https://www.reddit.com/r/embedded/comments/16wk7my/what_is_your_opinion_on_the_black_magic_debugger/

