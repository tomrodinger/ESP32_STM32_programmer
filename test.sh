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

hr() {
  printf '%s\n' '================================================================================'
}

banner() {
  # Highly visible headings for long outputs.
  # Usage: banner "TITLE" "optional subtitle"
  local title="$1"
  local subtitle="${2:-}"
  printf '\n'
  hr
  printf 'TEST: %s\n' "$title"
  if [[ -n "$subtitle" ]]; then
    printf 'PURPOSE: %s\n' "$subtitle"
  fi
  hr
}

success_box() {
  # Usage: success_box "STEP_NAME" "how we determined success"
  local name="$1"
  local how="$2"
  printf '\n'
  printf '********** SUCCESS: %s **********\n' "$name"
  printf '* DETERMINATION: %s\n' "$how"
  printf '*********************************\n'
  printf '\n'
}

print_command() {
  # Usage: print_command cmd arg1 arg2 ...
  printf 'RUNNING COMMAND:'
  local arg
  for arg in "$@"; do
    printf ' %q' "$arg"
  done
  printf '\n'
}

run_and_tee() {
  # Run a command, streaming output to stdout while also saving it to a log.
  # Sets global RUN_AND_TEE_RC to the command exit code.
  local log_path="$1"; shift

  : >"$log_path"

  # With set -euo pipefail enabled, pipelines can cause an immediate exit.
  # Temporarily disable -e so we can capture the true command exit code.
  set +e
  ( "$@" ) 2>&1 | tee "$log_path"
  RUN_AND_TEE_RC=${PIPESTATUS[0]}
  set -e
}

run_step() {
  local name="$1"; shift
  local purpose="$1"; shift
  local log="$TMP_LOG_DIR/${name}.log"

  banner "$name" "$purpose"
  print_command "$@"
  run_and_tee "$log" "$@"
  if [[ "$RUN_AND_TEE_RC" -ne 0 ]]; then
    fail "$name (command exit code: $RUN_AND_TEE_RC)" "$log"
  fi
  success_box "$name" "command exited with code 0"
}

run_step_expect() {
  local name="$1"; shift
  local purpose="$1"; shift
  local expect="$1"; shift
  local log="$TMP_LOG_DIR/${name}.log"

  banner "$name" "$purpose"
  print_command "$@"
  run_and_tee "$log" "$@"
  if [[ "$RUN_AND_TEE_RC" -ne 0 ]]; then
    fail "$name (command exit code: $RUN_AND_TEE_RC)" "$log"
  fi

  local match
  match="$(grep -Fnm1 -- "$expect" "$log" || true)"
  if [[ -z "$match" ]]; then
    fail "$name (missing expected text: $expect)" "$log"
  fi
  success_box "$name" "saw expected text via grep -F on log: '$expect' (first match: $match)"
}

run_step_no_warnings() {
  local name="$1"; shift
  local purpose="$1"; shift
  local log="$TMP_LOG_DIR/${name}.log"

  banner "$name" "$purpose"
  print_command "$@"
  run_and_tee "$log" "$@"
  if [[ "$RUN_AND_TEE_RC" -ne 0 ]]; then
    fail "$name (command exit code: $RUN_AND_TEE_RC)" "$log"
  fi

  # Treat any compiler warnings as failure.
  local warn_re="(^|\\s)(warning:|\\[WARNING\\])"
  local warn_match
  warn_match="$(grep -Einm1 "$warn_re" "$log" || true)"
  if [[ -n "$warn_match" ]]; then
    fail "$name (compiler warnings detected; first match: $warn_match)" "$log"
  fi

  success_box "$name" "command exited with code 0 AND no warnings matched regex: $warn_re"
}

if [[ ! -x "$ROOT_DIR/build_and_upload.py" ]]; then
  fail "build_and_upload.py is not executable" ""
fi

# 1) Build + upload filesystem image (SPIFFS) using the selected host bootloader*.bin.
run_step \
  "fs_build_upload" \
  "Build + upload filesystem image (SPIFFS) using selected host bootloader*.bin" \
  "$ROOT_DIR/build_and_upload.py" --skip-firmware

# 2) Compile firmware with no warnings.
run_step_no_warnings \
  "fw_compile" \
  "Compile firmware; treat any warnings as failures" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-upload

# 3) Upload firmware.
run_step \
  "fw_upload" \
  "Upload compiled firmware to the device" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build

# 4) Device command checks (do not rebuild/reupload).
run_step_expect \
  "cmd_F" \
  "Select firmware file / confirm firmware selection path is valid" \
  "Firmware file selection OK" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -F
run_step_expect \
  "cmd_f" \
  "Check filesystem status output is present" \
  "Filesystem status:" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -f
run_step_expect \
  "cmd_i" \
  "Read target IDCODE (basic SWD communication sanity check)" \
  "IDCODE" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -i

# 4b) Serial log sync + write increments
run_step_expect \
  "cmd_s" \
  "Consume a serial and append it to consumed-serial record (test only)" \
  "Consume serial" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -s

# If serial is not set yet, set it to a known value so subsequent tests are deterministic.
run_step_expect \
  "cmd_set_serial" \
  "Set next serial to a known value for testing (appends USERSET_<N>)" \
  "Set serial OK" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload --set-serial 1000

run_step_expect \
  "cmd_w_serial_1" \
  "Write once and confirm it prints the serial it will use" \
  "Write will use serial=" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -w --max 180 --quiet 1.0

run_step_expect \
  "cmd_w_serial_2" \
  "Write again and confirm it prints the serial it will use (should increment)" \
  "Write will use serial=" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -w --max 180 --quiet 1.0

# Confirm the serial number increments by 1 between the two w runs.
{
  log1="$TMP_LOG_DIR/cmd_w_serial_1.log"
  log2="$TMP_LOG_DIR/cmd_w_serial_2.log"
  s1="$(grep -Eo "Write will use serial=[0-9]+" "$log1" | head -n1 | sed -E 's/.*serial=([0-9]+)/\1/')"
  s2="$(grep -Eo "Write will use serial=[0-9]+" "$log2" | head -n1 | sed -E 's/.*serial=([0-9]+)/\1/')"
  if [[ -z "$s1" || -z "$s2" ]]; then
    fail "cmd_w_serial_increment (could not extract serial numbers from logs)" "$log2"
  fi
  if [[ $((s1 + 1)) -ne "$s2" ]]; then
    fail "cmd_w_serial_increment (expected s2 == s1+1; got s1=$s1 s2=$s2)" "$log2"
  fi
  success_box "cmd_w_serial_increment" "serial incremented by 1 (s1=$s1 -> s2=$s2)"
}

run_step_expect \
  "cmd_l" \
  "Print the full serial log to Serial" \
  "--- /log.txt ---" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -l
run_step_expect \
  "cmd_e" \
  "Erase target flash and confirm completion" \
  "Erase OK" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -e --max 120 --quiet 1.0
run_step_expect \
  "cmd_w" \
  "Write firmware to target flash and confirm completion" \
  "Write OK" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -w --max 180 --quiet 1.0
run_step_expect \
  "cmd_v" \
  "Verify written flash contents match expected image" \
  "Verify OK" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload -v --max 180 --quiet 1.0

# 5) Full production sequence (<space>). This is allowed to take longer.
run_step_expect \
  "cmd_space" \
  "Run full production sequence and confirm it reports success" \
  "PRODUCTION sequence SUCCESS" \
  "$PY" "$ROOT_DIR/tools/esp32_runner.py" --skip-build --skip-upload --space --max 240 --quiet 1.0
