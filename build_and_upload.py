#!/usr/bin/env python3

"""Build + upload fwfs (SPIFFS) image and build + upload ESP32 firmware.

Policy for selecting the firmware file that goes into the LittleFS image:

  - search for files matching "bootloader*.bin" in the *project root* (this folder)
  - exactly one match must exist, otherwise exit non-zero with a clear error
  - the file is copied into a temporary directory and used as the filesystem image input
    so we do not need to modify the repo's `data/` directory.

This script then:
  1) pio run -t buildfwfs
  2) pio run -t uploadfwfs
  3) python3 tools/esp32_runner.py  (build + upload firmware; plus any extra args you pass)

Examples:
  python3 build_and_upload.py
  python3 build_and_upload.py -- --space --max 120 --quiet 1.0
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def _die(msg: str, exit_code: int = 2) -> "NoReturn":
    sys.stderr.write(f"ERROR: {msg}\n")
    raise SystemExit(exit_code)


def _run(cmd: list[str], *, env: dict[str, str] | None = None) -> None:
    sys.stdout.write(f"\n[cmd] {' '.join(cmd)}\n")
    sys.stdout.flush()
    subprocess.run(cmd, check=True, env=env)


def _find_unique_host_bootloader_bin(project_root: Path) -> Path:
    # Host-side naming can be long; we stage it into SPIFFS under a short name
    # due to SPIFFS_OBJ_NAME_LEN limits.
    matches = sorted(p for p in project_root.glob("bootloader*.bin") if p.is_file())

    if not matches:
        _die(
            "No firmware file found. Expected exactly one file matching 'bootloader*.bin' in the project root."
        )
    if len(matches) > 1:
        listed = "\n".join(f"  - {p.name}" for p in matches)
        _die(
            "Multiple firmware files found in the project root matching 'bootloader*.bin'.\n"
            "Remove/rename extras so exactly one remains, then retry.\n\n"
            f"Matches:\n{listed}"
        )
    return matches[0]


def _spiffs_name_for_host_bin(host_bin: Path) -> str:
    """Map a host bootloader*.bin into a <= 31 char SPIFFS filename.

    Toolchain mkspiffs reports SPIFFS_OBJ_NAME_LEN=32 (including the null terminator).
    Keep the on-device filename <= 31 characters.
    """

    # SPIFFS stores a fixed-size name field. mkspiffs in this toolchain reports:
    #   SPIFFS_OBJ_NAME_LEN: 32
    # That is the buffer size including the null terminator, so the usable max
    # filename length is 31 characters.
    spiffs_max_name_len = 31

    stem = host_bin.stem  # e.g. bootloader_M17_hw1.5_scc3_1766404965

    # Preserve underscores for readability, but shorten the leading constant.
    if stem.startswith("bootloader"):
        stem = "BL" + stem[len("bootloader") :]

    # Ensure the device selection policy (BL*) will find it.
    if not stem.startswith("BL"):
        _die(
            f"Selected host file {host_bin.name!r} does not map to a SPIFFS name starting with 'BL'. "
            f"Got candidate name {stem!r}."
        )

    if len(stem) > spiffs_max_name_len:
        _die(
            "SPIFFS filename too long after shortening.\n"
            f"  host file: {host_bin.name}\n"
            f"  candidate on-device name: {stem}\n"
            f"  candidate length: {len(stem)} characters\n"
            f"  SPIFFS limit: {spiffs_max_name_len} characters (SPIFFS_OBJ_NAME_LEN=32 incl null)\n"
            "Please shorten the host filename (especially the suffix) so the on-device name fits."
        )

    return stem


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Build+upload the fwfs LittleFS image using the unique bootloader*.bin in the project root, "
            "then build+upload the ESP32 firmware via tools/esp32_runner.py."
        )
    )
    ap.add_argument(
        "--skip-fwfs",
        action="store_true",
        help="Skip building/uploading the fwfs LittleFS image.",
    )
    ap.add_argument(
        "--skip-firmware",
        action="store_true",
        help="Skip building/uploading the ESP32 firmware (tools/esp32_runner.py).",
    )
    ap.add_argument(
        "runner_args",
        nargs=argparse.REMAINDER,
        help=(
            "Args passed through to tools/esp32_runner.py. If you need to pass flags starting with '-', "
            "separate with '--', e.g. build_and_upload.py -- --space --max 120."
        ),
    )

    args = ap.parse_args(argv)

    project_root = Path(__file__).resolve().parent
    os.chdir(project_root)

    env = os.environ.copy()

    if not args.skip_fwfs:
        fw_bin = _find_unique_host_bootloader_bin(project_root)
        spiffs_name = _spiffs_name_for_host_bin(fw_bin)
        sys.stdout.write(
            f"[info] Selected host firmware: {fw_bin.name}\n"
            f"[info] Staging into SPIFFS as: {spiffs_name}\n"
        )

        # Build the LittleFS image from a temporary directory containing only the selected file.
        with tempfile.TemporaryDirectory(prefix=".fwfs_data_", dir=str(project_root)) as tmpdir:
            tmp_path = Path(tmpdir)
            shutil.copy2(fw_bin, tmp_path / spiffs_name)
            env["FWFS_DATA_DIR"] = str(tmp_path)

            _run(["pio", "run", "-t", "buildfwfs"], env=env)
            _run(["pio", "run", "-t", "uploadfwfs"], env=env)

    if not args.skip_firmware:
        runner_argv = list(args.runner_args)
        if runner_argv and runner_argv[0] == "--":
            runner_argv = runner_argv[1:]
        _run([sys.executable, "tools/esp32_runner.py", *runner_argv])

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
