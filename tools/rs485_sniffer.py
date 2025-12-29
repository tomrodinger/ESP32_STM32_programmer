#!/usr/bin/env python3

"""RS485 sniffer: read raw bytes from a serial port and print them in hex.

This is intended for debugging the Servomotor firmware upgrade packets on the wire.

Example:
  python3 tools/rs485_sniffer.py --port /dev/tty.usbserial-210 --baud 230400
"""

from __future__ import annotations

import argparse
import sys
import time


def hex_line(chunk: bytes) -> str:
    return " ".join(f"{b:02X}" for b in chunk)


def now_s() -> float:
    # Prefer monotonic clock for gaps/relative timing.
    return time.monotonic()


def main() -> int:
    ap = argparse.ArgumentParser(description="Sniff and hex-dump bytes received on a serial port")
    ap.add_argument("--port", default="/dev/tty.usbserial-210", help="Serial device path")
    ap.add_argument("--baud", type=int, default=230400, help="Baud rate")
    ap.add_argument(
        "--read-size",
        type=int,
        default=4096,
        help="Max bytes per read() call (controls batching)",
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=1.0,
        help="Read timeout seconds (controls responsiveness)",
    )
    ap.add_argument(
        "--packet-gap-reset",
        type=float,
        default=1.0,
        help="If gap between bytes exceeds this many seconds, reset packet start time",
    )
    args = ap.parse_args()

    try:
        import serial  # type: ignore
    except Exception as e:
        print("ERROR: pyserial not installed.")
        print("Install into a venv, for example:")
        print("  python3 -m venv .venv")
        print("  source .venv/bin/activate")
        print("  pip install -r tools/requirements.txt")
        print(f"Import error: {e}")
        return 2

    try:
        ser = serial.Serial(
            port=args.port,
            baudrate=args.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=args.timeout,
        )
    except Exception as e:
        print(f"ERROR: failed to open port {args.port}: {e}")
        return 2

    print(f"Sniffing {args.port} @ {args.baud} baud. Ctrl-C to stop.")
    total = 0

    # Write space-separated timing data for offline analysis.
    data_path = "sniffer_timing_data.txt"
    data_f = open(data_path, "w", encoding="utf-8")
    data_f.write("time_since_start_of_packet_ms gap_ms max_gap_ms byte\n")

    # Packet-relative timing state:
    packet_t0: float | None = None
    last_byte_t: float | None = None
    max_gap_s = 0.0

    try:
        # Tight loop: read 1 byte at a time and timestamp its arrival.
        # We keep a packet-relative time origin that resets if there's a long gap.
        while True:
            b = ser.read(1)
            if not b:
                continue

            t = now_s()
            reset_packet = False
            if last_byte_t is None:
                # First byte ever observed.
                packet_t0 = t
                max_gap_s = 0.0
                gap_s = 0.0
                reset_packet = True
            else:
                gap_s = t - last_byte_t
                if gap_s > max_gap_s:
                    max_gap_s = gap_s
                if gap_s > args.packet_gap_reset:
                    # Consider this a new packet: reset origin and max-gap stat.
                    packet_t0 = t
                    max_gap_s = 0.0
                    reset_packet = True

            last_byte_t = t
            assert packet_t0 is not None
            rel_ms = (t - packet_t0) * 1000.0
            gap_ms = gap_s * 1000.0
            max_gap_ms = max_gap_s * 1000.0

            # If this byte started a new packet, force the reported gap to 0.
            if reset_packet:
                gap_ms = 0.0

            total += 1
            sys.stdout.write(
                f"{rel_ms:9.3f} ms  gap={gap_ms:8.3f} ms  max_gap={max_gap_ms:8.3f} ms  {b[0]:02X}\n"
            )
            sys.stdout.flush()

            # Also write a simple space-separated row for plotting.
            data_f.write(f"{rel_ms:.3f} {gap_ms:.3f} {max_gap_ms:.3f} {b[0]:02X}\n")
            # Keep file reasonably up-to-date if user kills the program.
            if (total % 256) == 0:
                data_f.flush()
    except KeyboardInterrupt:
        # Allow Ctrl-C to stop without a noisy traceback.
        try:
            ser.close()
        except Exception:
            pass
        try:
            data_f.flush()
            data_f.close()
        except Exception:
            pass

    print(f"Stopped. Total bytes: {total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
