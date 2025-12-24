Import("env")

import os
import time


def _parse_int(s: str) -> int:
    s = str(s).strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 10)


def _find_partition(partitions_csv: str, name: str):
    # Very small CSV parser (same style as Arduino/PlatformIO):
    # Name, Type, SubType, Offset, Size, Flags
    # Ignore comments and blank lines.
    with open(partitions_csv, "r", encoding="utf-8") as f:
        for raw in f.readlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            tokens = [t.strip() for t in line.split(",")]
            if len(tokens) < 5:
                continue
            if tokens[0] != name:
                continue
            return {
                "name": tokens[0],
                "type": tokens[1],
                "subtype": tokens[2],
                "offset": _parse_int(tokens[3]),
                "size": _parse_int(tokens[4]),
            }
    raise RuntimeError(f"Partition '{name}' not found in {partitions_csv}")


def _configure_fwfs(env):
    partitions_csv = env.subst("$PARTITIONS_TABLE_CSV")
    p = _find_partition(partitions_csv, "fwfs")
    # Use hex strings so SCons variable substitution is reliable everywhere.
    # (We later embed these into CLI strings.)
    start_hex = f"0x{p['offset']:X}"
    env.Replace(
        FWFS_START=start_hex,
        FWFS_SIZE=str(p["size"]),
        FWFS_PAGE=0x100,
        FWFS_BLOCK=0x1000,
        # Allow overriding the input directory used to build the LittleFS image.
        # This is useful for automation scripts that want to stage exactly one
        # bootloader*.bin without modifying the repo's `data/` directory.
        FWFS_DATA_DIR=os.environ.get("FWFS_DATA_DIR", env.subst("$PROJECT_DATA_DIR")),
        FWFS_IMAGE=os.path.join(env.subst("$BUILD_DIR"), "fwfs.bin"),
    )

    # PlatformIO may not populate a dedicated SPIFFS image tool var. Provide a
    # robust fallback using the installed package path.
    try:
        mkspiffs = env.subst("$MKFSPIFFSTOOL")
    except Exception:
        mkspiffs = ""
    if not mkspiffs or mkspiffs.strip() in ("\"\"", ""):
        mkspiffs = os.path.join(env.PioPlatform().get_package_dir("tool-mkspiffs"), "mkspiffs_espressif32_arduino")
    env.Replace(MKFSPIFFSTOOL=mkspiffs)


def _build_fwfs(target, source, env):
    _configure_fwfs(env)
    env.Execute(
        env.VerboseAction(
            # Use the SPIFFS mkspiffs tool explicitly (LittleFS mklittlefs is unreliable on some macOS arm64 setups).
            '"$MKFSPIFFSTOOL" -c "$FWFS_DATA_DIR" -s $FWFS_SIZE -p $FWFS_PAGE -b $FWFS_BLOCK "$FWFS_IMAGE"',
            "Building FWFS SPIFFS image",
        )
    )


def _upload_fwfs(target, source, env):
    _configure_fwfs(env)
    # Ensure image exists
    if not os.path.isfile(env.subst("$FWFS_IMAGE")):
        _build_fwfs(target, source, env)

    # Use esptool directly, similar to PlatformIO's built-in `uploadfs`.
    # We intentionally do NOT reuse $UPLOADERFLAGS because those are intended for
    # flashing the main firmware image and may include offsets that don't apply.
    env.AutodetectUploadPort()

    board_cfg = env.BoardConfig()
    before_reset = board_cfg.get("upload.before_reset", "default_reset")
    after_reset = board_cfg.get("upload.after_reset", "hard_reset")

    # Build the command string explicitly (avoid relying on $UPLOAD_RESETMETHOD,
    # which may be unset in some PlatformIO/board combinations).
    cmd = (
        '"$PYTHONEXE" "$UPLOADER" '
        "--chip $BOARD_MCU "
        "--port \"$UPLOAD_PORT\" "
        "--baud $UPLOAD_SPEED "
        f"--before {before_reset} "
        f"--after {after_reset} "
        "write_flash -z "
        "--flash_mode ${__get_board_flash_mode(__env__)} "
        "--flash_freq ${__get_board_f_image(__env__)} "
        # Do not pass --flash_size here; esptool accepts only symbolic sizes
        # and auto-detect is robust.
        "$FWFS_START $FWFS_IMAGE"
    )

    # On macOS, the USB CDC device node can disappear/reappear across resets.
    # Autodetect + retry a few times to avoid transient "port doesn't exist" failures.
    last_rc = 0
    for attempt in range(1, 6):
        env.AutodetectUploadPort()
        last_rc = env.Execute(env.VerboseAction(cmd, f"Uploading FWFS SPIFFS image (attempt {attempt}/5)"))
        if not last_rc:
            break
        time.sleep(0.6)
    if last_rc:
        raise RuntimeError(f"esptool failed with exit code {last_rc}")


# Register custom targets
env.AddCustomTarget(
    name="buildfwfs",
    dependencies=None,
    actions=[_build_fwfs],
    title="Build fwfs SPIFFS image",
    description="Build a SPIFFS image for the 'fwfs' partition.",
)

env.AddCustomTarget(
    name="uploadfwfs",
    dependencies=None,
    actions=[_upload_fwfs],
    title="Upload fwfs SPIFFS image",
    description="Flash the fwfs SPIFFS image to the device at the partition's offset.",
)
