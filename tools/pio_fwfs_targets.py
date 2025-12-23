Import("env")

import os


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
        FWFS_IMAGE=os.path.join(env.subst("$BUILD_DIR"), "fwfs.bin"),
    )


def _build_fwfs(target, source, env):
    _configure_fwfs(env)
    env.Execute(
        env.VerboseAction(
            '"$MKFSTOOL" -c "$PROJECT_DATA_DIR" -s $FWFS_SIZE -p $FWFS_PAGE -b $FWFS_BLOCK "$FWFS_IMAGE"',
            "Building FWFS LittleFS image",
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

    rc = env.Execute(env.VerboseAction(cmd, "Uploading FWFS LittleFS image"))
    if rc:
        raise RuntimeError(f"esptool failed with exit code {rc}")


# Register custom targets
env.AddCustomTarget(
    name="buildfwfs",
    dependencies=None,
    actions=[_build_fwfs],
    title="Build fwfs LittleFS image",
    description="Build a LittleFS image for the 'fwfs' partition (subtype 0x83).",
)

env.AddCustomTarget(
    name="uploadfwfs",
    dependencies=None,
    actions=[_upload_fwfs],
    title="Upload fwfs LittleFS image",
    description="Flash the fwfs LittleFS image to the device at the partition's offset.",
)
