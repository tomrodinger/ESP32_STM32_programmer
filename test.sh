#!/bin/bash
set -euo pipefail

# Test runner for this repo.
#
# Contract:
# - After each test step passes, print a single line: SUCCESS
# - If a step fails, print its captured log and exit non-zero.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

PY="python3"
if [[ -x "$ROOT_DIR/.venv/bin/python3" ]]; then
  PY="$ROOT_DIR/.venv/bin/python3"
fi

TMP_LOG_DIR="$ROOT_DIR/.test_logs"
rm -rf "$TMP_LOG_DIR"
mkdir -p "$TMP_LOG_DIR"

fail() {
  local msg="$1"
  local log_path="${2:-}"
  echo "FAIL: $msg" 1>&2
  if [[ -n "$log_path" && -f "$log_path" ]]; then
    echo "----- LOG ($log_path) -----" 1>&2
    cat "$log_path" 1>&2
    echo "----- END LOG -----" 1>&2
  fi
  exit 1
}

run_step() {
  local name="$1"; shift
  local log="$TMP_LOG_DIR/${name}.log"
  ( "$@" ) >"$log" 2>&1 || fail "$name" "$log"
  echo SUCCESS
}

run_step_expect() {
  local name="$1"; shift
  local expect="$1"; shift
  local log="$TMP_LOG_DIR/${name}.log"
  ( "$@" ) >"$log" 2>&1 || fail "$name" "$log"
  if ! grep -Fq "$expect" "$log"; then
    fail "$name (missing expected text: $expect)" "$log"
  fi
  echo SUCCESS
}

run_step_no_warnings() {
  local name="$1"; shift
  local log="$TMP_LOG_DIR/${name}.log"
  ( "$@" ) >"$log" 2>&1 || fail "$name" "$log"
  # Treat any compiler warnings as failure.
  if grep -Eiq "(^|\s)(warning:|\[WARNING\])" "$log"; then
    fail "$name (compiler warnings detected)" "$log"
  fi
  echo SUCCESS
}

if [[ ! -x "$ROOT_DIR/build_and_upload.py" ]]; then
  fail "build_and_upload.py is not executable" ""
fi

# 1) Build + upload filesystem image (SPIFFS) using the selected host bootloader*.bin.
run_step "fs_build_upload" "$ROOT_DIR/build_and_upload.py" --skip-firmware

# 2) Compile firmware with no warnings.
run_step_no_warnings "fw_compile" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-upload

# 3) Upload firmware.
run_step "fw_upload" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build

# 4) Device command checks (do not rebuild/reupload).
run_step_expect "cmd_F" "Firmware file selection OK" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -F
run_step_expect "cmd_f" "Filesystem status:" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -f
run_step_expect "cmd_i" "IDCODE" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -i
run_step_expect "cmd_e" "Erase OK" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -e --max 120 --quiet 1.0
run_step_expect "cmd_w" "Write OK" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -w --max 180 --quiet 1.0
run_step_expect "cmd_v" "Verify OK" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -v --max 180 --quiet 1.0

# 5) Full production sequence (<space>). This is allowed to take longer.
run_step_expect "cmd_space" "PRODUCTION sequence SUCCESS" "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload --space --max 240 --quiet 1.0

