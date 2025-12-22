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
# NOTE: We intentionally gate which commands can be sent to avoid accidental
# destructive actions when iterating.
#
# Production sequence is bound to the spacebar (ASCII 0x20). We support it via
# the explicit `--space` flag (rather than a short `- ` option).
ALLOWED_CMDS = set("hidbtcpermwva") | {"r", "p", " "}


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
            return str(port)

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
            return serial.Serial(port=port, baudrate=baud, timeout=0.05)
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


def _send_cmd_and_capture(ser: "serial.Serial", cmd_char: str, quiet_s: float, max_s: float) -> None:
    # Send just the character + newline. Firmware ignores whitespace.
    payload = (cmd_char + "\n").encode("utf-8")
    ser.write(payload)
    ser.flush()

    start = time.time()
    last_rx = time.time()
    while True:
        data = ser.read(4096)
        now = time.time()

        if data:
            _OUT.write(data.decode("utf-8", errors="replace"))
            _OUT.flush()
            last_rx = now
        else:
            time.sleep(0.02)

        if now - start >= max_s:
            break
        if now - last_rx >= quiet_s:
            break


def _parse_cmds(argv: List[str]) -> List[str]:
    """Extract ordered command characters from argv.

    Preserves ordering (left-to-right) to match how a human would type sequences.
    """

    cmds: List[str] = []
    for a in argv:
        if a == "--space":
            cmds.append(" ")
            continue

        if a.startswith("-") and not a.startswith("--") and len(a) == 2:
            c = a[1]
            if c in ALLOWED_CMDS:
                cmds.append(c)

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
    boot_wait_s = 2.0
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
            )
            raise SystemExit(0)
        i += 1

    cmds = _parse_cmds(argv)
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
        _drain_and_print(ser, duration_s=0.5)

        for c in args.cmds:
            _OUT.write(f"\n[send] {c}\n")
            _OUT.flush()
            _send_cmd_and_capture(ser, cmd_char=c, quiet_s=args.quiet_s, max_s=args.max_per_cmd_s)

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
