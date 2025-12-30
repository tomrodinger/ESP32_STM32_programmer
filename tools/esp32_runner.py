#!/usr/bin/env python3

"""Build + upload (PlatformIO) and then run serial commands against the ESP32.

Example:
  python3 tools/esp32_runner.py -i -r

This will:
  1) pio run
  2) pio run -t upload
  3) open the serial port at 115200
  4) send 'i' then 'r'
  5) print all serial output and exit
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import List, Optional


try:
    import serial  # type: ignore
except Exception as e:  # pragma: no cover
    sys.stderr.write(
        "ERROR: pyserial is required to run this helper.\n"
        "Create a local venv and install dependencies:\n\n"
        "  python3 -m venv .venv\n"
        "  source .venv/bin/activate\n"
        "  pip install -r tools/requirements.txt\n\n"
    )
    raise


# Keep in sync with the command table printed by the ESP32 firmware.
# NOTE: This helper intentionally preserves the left-to-right ordering of
# command flags (e.g. `-w -R --to-mode2 -p`).


def _run(cmd: List[str]) -> None:
    _OUT.write(f"\n[cmd] {' '.join(cmd)}\n")
    _OUT.flush()
    subprocess.run(cmd, check=True)


def _pio_device_list() -> List[dict]:
    # PlatformIO supports JSON output.
    p = subprocess.run(
        ["pio", "device", "list", "--json-output"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return json.loads(p.stdout)


def _choose_port(devices: List[dict]) -> str:
    # Heuristic: pick the first device that looks like a USB serial device.
    for d in devices:
        port = d.get("port")
        if not port:
            continue
        desc = (d.get("description") or "").lower()
        hwid = (d.get("hwid") or "").lower()
        if "usb" in desc or "usb" in hwid or "cdc" in desc or "uart" in desc:
            p = str(port)
            # On macOS, prefer /dev/tty.* over /dev/cu.*. The /dev/cu.* device can
            # cause DTR/RTS side effects that reset some ESP32-S3 setups into ROM
            # download mode.
            if p.startswith("/dev/cu."):
                return "/dev/tty." + p[len("/dev/cu.") :]
            return p

    # Fallback: if there is exactly one device, use it.
    if len(devices) == 1 and devices[0].get("port"):
        return str(devices[0]["port"])

    raise RuntimeError(
        "Could not automatically choose a serial port. "
        "Use --port /dev/tty.* (macOS) to specify it explicitly."
    )


def _open_serial_with_retry(port: str, baud: int, open_timeout_s: float) -> "serial.Serial":
    deadline = time.time() + open_timeout_s
    last_err: Optional[Exception] = None
    while time.time() < deadline:
        try:
            # Keep the open path minimal; some ESP32-S3 USB-CDC stacks are sensitive
            # to DTR/RTS manipulation and can reset into ROM download mode.
            ser = serial.Serial(port=port, baudrate=baud, timeout=0.05)
            # Best-effort: leave lines deasserted.
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            return ser
        except Exception as e:
            last_err = e
            time.sleep(0.25)
    raise RuntimeError(f"Failed to open serial port {port!r}: {last_err}")


def _drain_and_print(ser: "serial.Serial", duration_s: float) -> None:
    end = time.time() + duration_s
    while time.time() < end:
        data = ser.read(4096)
        if data:
            _OUT.write(data.decode("utf-8", errors="replace"))
            _OUT.flush()
        else:
            time.sleep(0.02)


def _drain_capture(ser: "serial.Serial", duration_s: float) -> str:
    end = time.time() + duration_s
    out = ""
    while time.time() < end:
        data = ser.read(4096)
        if data:
            out += data.decode("utf-8", errors="replace")
        else:
            time.sleep(0.02)
    return out


def _looks_like_rom_download(s: str) -> bool:
    ss = s.lower()
    return ("esp-rom" in ss) or ("waiting for download" in ss) or ("download(usb" in ss)


def _looks_like_firmware(s: str) -> bool:
    # Keep markers fairly loose; banners can vary across builds.
    return (
        "ESP32-S3 STM32G0 Programmer" in s
        or "Mounting SPIFFS" in s
        or "Filesystem status:" in s
        or "MODE 2" in s
    )


def _try_reset_sequences(ser: "serial.Serial") -> None:
    # Try several DTR/RTS combinations. Different boards/USB bridges invert these.
    # The goal is to get out of ROM download mode and into app.
    sequences = [
        # (dtr, rts, pulse_reset)
        (False, False, True),
        (True, False, True),
        (False, True, True),
        (True, True, True),
    ]

    for dtr, rts, pulse in sequences:
        try:
            ser.dtr = dtr
            ser.rts = rts
        except Exception:
            # If the platform doesn't support toggling, there's nothing else we can do.
            return

        if pulse:
            # Pulse RTS as "reset" best-effort (common on ESP auto-reset circuits).
            try:
                ser.rts = True
                time.sleep(0.12)
                ser.rts = False
            except Exception:
                pass

        # Give the target time to reboot.
        time.sleep(0.25)

        # Drain any boot output so callers can inspect it.
        s = _drain_capture(ser, duration_s=0.8)
        if s:
            _OUT.write(s)
            _OUT.flush()

        if _looks_like_firmware(s):
            return


def _send_cmd_and_capture(ser: "serial.Serial", cmd_char: str, quiet_s: float, max_s: float, mode: int) -> None:
    # Send the command + newline.
    # Some commands carry an inline argument (e.g. "S1234").
    payload = (cmd_char + "\n").encode("utf-8")
    ser.write(payload)
    ser.flush()

    start = time.time()
    last_rx = time.time()

    # Some commands can be long-running and may legitimately produce no output
    # for extended periods (e.g. verify/program). For these, we avoid the
    # quiet-time early exit and instead stop when we see an expected terminal
    # marker or when max_s elapses.
    stop_markers: List[str] = []
    lead = cmd_char[0] if cmd_char else ""

    if mode == 1:
        if lead == "e":
            stop_markers = ["Erase OK", "Erase FAIL"]
        elif lead == "w":
            stop_markers = ["Write OK", "Write FAIL"]
        elif lead == "v":
            stop_markers = ["Verify OK", "Verify FAIL"]
        elif lead == "s":
            stop_markers = ["Consume serial OK:", "Consume serial FAIL"]
        elif lead == "l":
            # Wait for the full output of both sections.
            # If we stop at the section header, we can truncate output mid-command.
            stop_markers = ["--- END /serial_consumed.bin ---", "Log open FAIL"]
        elif lead == "a":
            stop_markers = ["WiFi mode:", "WiFi AP IP:", "WiFi AP enabled:"]
        elif lead == "R":
            stop_markers = ["Pulsing NRST LOW", "Prep OK", "Prep FAIL"]
        elif lead == "S":
            stop_markers = ["Set serial OK:", "Set serial FAIL", "Set serial:"]
        elif lead == " ":
            stop_markers = ["PRODUCTION sequence SUCCESS", "Production sequence aborted", "ERROR: Production sequence aborted"]
        elif lead == "F":
            stop_markers = ["Firmware file selection OK", "Firmware file selection FAIL"]
        elif lead == "f":
            stop_markers = ["Filesystem status:"]
        elif lead == "i":
            stop_markers = ["DP IDCODE:", "DP IDCODE read failed"]
        elif lead == "u":
            stop_markers = ["Upgrade OK", "Upgrade FAIL", "Servomotor upgrade OK"]
        elif lead == "p":
            stop_markers = ["Servomotor GET_PRODUCT_INFO response:", "ERROR: getProductInfo"]
    else:
        # Mode 2 (RS485 testing)
        if lead == "p":
            stop_markers = [
                "Servomotor GET_COMPREHENSIVE_POSITION response",
                "ERROR: getComprehensivePosition",
                "ERROR: DUT unique_id not known",
            ]
        elif lead == "P":
            stop_markers = [
                "Servomotor GET_COMPREHENSIVE_POSITION response",
                "ERROR: getComprehensivePosition(ref)",
            ]
        elif lead == "i":
            stop_markers = ["Servomotor GET_PRODUCT_INFO response:", "ERROR: getProductInfo", "ERROR: DUT unique_id not known"]
        elif lead == "e":
            stop_markers = ["[Motor] enableMosfets called.", "ERROR: enableMosfets", "ERROR: DUT unique_id not known"]
        elif lead == "d":
            stop_markers = ["[Motor] disableMosfets called.", "ERROR: disableMosfets", "ERROR: DUT unique_id not known"]
        elif lead == "t":
            stop_markers = ["[Motor] trapezoidMove", "ERROR: trapezoidMove", "ERROR: DUT unique_id not known"]
        elif lead == "R":
            stop_markers = ["[Motor] systemReset called.", "ERROR: systemReset", "ERROR: DUT unique_id not known"]
        elif lead == "s":
            stop_markers = ["Mode2 getStatus OK", "Mode2 getStatus FAIL", "ERROR: getStatus", "ERROR: DUT unique_id not known"]
        elif lead == "v":
            stop_markers = [
                "Mode2 getSupplyVoltage OK",
                "Mode2 getSupplyVoltage FAIL",
                "ERROR: getSupplyVoltage",
                "ERROR: DUT unique_id not known",
            ]
        elif lead == "c":
            stop_markers = [
                "Mode2 getTemperature OK",
                "Mode2 getTemperature FAIL",
                "ERROR: getTemperature",
                "ERROR: DUT unique_id not known",
            ]
        elif lead == "D":
            stop_markers = ["Detect devices:", "ERROR: detectDevices"]

    # Firmware upgrade is also long-running and can have multi-second stretches
    # without output (device flash write), so do not apply quiet-time early exit.
    long_running = lead in {"e", "w", "v", " ", "u"} if mode == 1 else lead in {"u", "p", "P", "i", "e", "d", "t", "R", "s", "v", "c", "D"}
    buf = ""
    while True:
        data = ser.read(4096)
        now = time.time()

        if data:
            s = data.decode("utf-8", errors="replace")
            _OUT.write(s)
            _OUT.flush()
            if stop_markers:
                buf = (buf + s)[-8192:]
                if any(m in buf for m in stop_markers):
                    break
            last_rx = now
        else:
            time.sleep(0.02)

        if now - start >= max_s:
            break
        if (not long_running) and (now - last_rx >= quiet_s):
            break


def _parse_cmds(argv: List[str], ignore_indices: "set[int]") -> List[str]:
    """Extract ordered command characters from argv.

    Preserves ordering (left-to-right) to match how a human would type sequences.
    """

    cmds: List[str] = []
    i = 0
    while i < len(argv):
        if i in ignore_indices:
            i += 1
            continue
        a = argv[i]
        if a == "--space":
            cmds.append(" ")
            i += 1
            continue

        if a == "--set-serial" and i + 1 < len(argv):
            n = str(argv[i + 1]).strip()
            if not n.isdigit():
                raise ValueError(f"--set-serial expects a uint32 decimal string, got: {n!r}")
            cmds.append("S" + n)
            i += 2
            continue

        if a.startswith("-") and not a.startswith("--") and len(a) == 2:
            # Backwards-compatible: -i -r etc.
            c = a[1]
            cmds.append(c)
            i += 1
            continue

        # New: allow raw command tokens too, e.g. "2" "p" "R" "e".
        # Accept single-char commands. Also accept "S123" style commands.
        if not a.startswith("-") and a:
            if len(a) == 1 or (len(a) >= 2 and a[0] == "S" and a[1:].isdigit()):
                cmds.append(a)
                i += 1
                continue

        i += 1

    return cmds


@dataclass
class Args:
    port: Optional[str]
    baud: int
    skip_build: bool
    skip_upload: bool
    log_path: str
    open_timeout_s: float
    boot_wait_s: float
    quiet_s: float
    max_per_cmd_s: float
    cmds: List[str]


def _parse_args(argv: List[str]) -> Args:
    # Keep it lightweight: parse a few --options manually, but preserve -i -r ordering
    # by scanning argv.
    port = None
    baud = 115200
    skip_build = False
    skip_upload = False
    log_path = "tools/esp32_runner_last.log"
    open_timeout_s = 15.0
    boot_wait_s = 4.0
    quiet_s = 0.6
    max_per_cmd_s = 6.0

    it = iter(range(len(argv)))
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--port" and i + 1 < len(argv):
            port = argv[i + 1]
            i += 2
            continue
        if a == "--baud" and i + 1 < len(argv):
            baud = int(argv[i + 1])
            i += 2
            continue
        if a == "--skip-build":
            skip_build = True
            i += 1
            continue
        if a == "--skip-upload":
            skip_upload = True
            i += 1
            continue
        if a == "--log" and i + 1 < len(argv):
            log_path = argv[i + 1]
            i += 2
            continue
        if a == "--open-timeout" and i + 1 < len(argv):
            open_timeout_s = float(argv[i + 1])
            i += 2
            continue
        if a == "--boot-wait" and i + 1 < len(argv):
            boot_wait_s = float(argv[i + 1])
            i += 2
            continue
        if a == "--quiet" and i + 1 < len(argv):
            quiet_s = float(argv[i + 1])
            i += 2
            continue
        if a == "--max" and i + 1 < len(argv):
            max_per_cmd_s = float(argv[i + 1])
            i += 2
            continue
        if a in ("-h", "--help"):
            sys.stdout.write(
                "Usage: python3 tools/esp32_runner.py [-i] [-r] ... [--port PORT]\n\n"
                "Examples:\n"
                "  python3 tools/esp32_runner.py -i\n"
                "  python3 tools/esp32_runner.py -i -r\n\n"
                "Options:\n"
                "  --port PORT         Serial port (e.g. /dev/tty.usbmodemXXXX)\n"
                "  --baud N            Baud rate (default: 115200)\n"
                "  --skip-build        Do not run 'pio run'\n"
                "  --skip-upload       Do not run 'pio run -t upload'\n"
                "  --log PATH          Write a full copy of output to PATH (default: tools/esp32_runner_last.log)\n"
                "  --open-timeout S    How long to wait for the serial port (default: 15)\n"
                "  --boot-wait S       Wait after opening port before sending commands (default: 2)\n"
                "  --quiet S           Consider command done after S seconds of no output (default: 0.6)\n"
                "  --max S             Max seconds to wait per command (default: 6)\n"
                "  --space            Send the production <space> command (ASCII 0x20)\n"
                "  --set-serial N     Send S<N> to set the next serial (append USERSET_<N>)\n"
            )
            raise SystemExit(0)
        i += 1

    ignore: set[int] = set()

    # Mark option value indices so they won't be mis-parsed as raw command tokens.
    # (e.g. `--boot-wait 5` should not send the command "5").
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in {"--port", "--baud", "--log", "--open-timeout", "--boot-wait", "--quiet", "--max", "--set-serial"}:
            if i + 1 < len(argv):
                ignore.add(i + 1)
            i += 2
            continue
        i += 1

    cmds = _parse_cmds(argv, ignore)
    return Args(
        port=port,
        baud=baud,
        skip_build=skip_build,
        skip_upload=skip_upload,
        log_path=log_path,
        open_timeout_s=open_timeout_s,
        boot_wait_s=boot_wait_s,
        quiet_s=quiet_s,
        max_per_cmd_s=max_per_cmd_s,
        cmds=cmds,
    )


class _Tee:
    def __init__(self, file_path: str):
        self._fp = open(file_path, "w", encoding="utf-8", errors="replace")

    def write(self, s: str) -> None:
        sys.stdout.write(s)
        self._fp.write(s)

    def flush(self) -> None:
        sys.stdout.flush()
        self._fp.flush()

    def close(self) -> None:
        try:
            self._fp.close()
        except Exception:
            pass


_OUT: _Tee


def main() -> int:
    args = _parse_args(sys.argv[1:])

    global _OUT
    _OUT = _Tee(args.log_path)
    _OUT.write(f"[info] Logging to: {args.log_path}\n")
    _OUT.flush()

    if not args.skip_build:
        _run(["pio", "run"])
    if not args.skip_upload:
        # If --port is provided, also use it as PlatformIO's upload port.
        # This helps when autodetection is flaky on macOS due to USB CDC re-enumeration.
        if args.port:
            _run(["pio", "run", "-t", "upload", "--upload-port", args.port])
        else:
            _run(["pio", "run", "-t", "upload"])

    if not args.cmds:
        _OUT.write("\nNo commands specified (e.g. -i -r). Nothing to do.\n")
        return 0

    port = args.port
    if not port:
        devs = _pio_device_list()
        port = _choose_port(devs)
        _OUT.write(f"\n[info] Auto-selected port: {port}\n")

    ser = _open_serial_with_retry(port=port, baud=args.baud, open_timeout_s=args.open_timeout_s)
    try:
        # Give USB-CDC and firmware some time to settle.
        time.sleep(args.boot_wait_s)
        boot_text = _drain_capture(ser, duration_s=0.7)
        if boot_text:
            _OUT.write(boot_text)
            _OUT.flush()

        if _looks_like_rom_download(boot_text) and not _looks_like_firmware(boot_text):
            _OUT.write("[warn] Detected ROM download mode on serial open; attempting reset sequences...\n")
            _OUT.flush()
            _try_reset_sequences(ser)

        mode = 1
        for c in args.cmds:
            _OUT.write(f"\n[send] {c}\n")
            _OUT.flush()
            if c == "1":
                mode = 1
            elif c == "2":
                mode = 2
            _send_cmd_and_capture(ser, cmd_char=c, quiet_s=args.quiet_s, max_s=args.max_per_cmd_s, mode=mode)

        return 0
    finally:
        try:
            ser.close()
        except Exception:
            pass

        try:
            _OUT.flush()
            _OUT.close()
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
