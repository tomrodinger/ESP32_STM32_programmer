#!/usr/bin/env python3

"""Compare two hex sniffer captures.

Supports captures formatted like:
  - "FF 1B 08 ..." (space-separated hex bytes)
  - "0xFF 0x1B ..." (0x-prefixed hex bytes)
  - arbitrary whitespace/newlines

Usage:
  python3 tools/compare_sniffs.py sniff1 sniff2
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass


TOKEN_RE = re.compile(r"(?:0x)?([0-9A-Fa-f]{2})")


def load_bytes(path: str) -> bytes:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    tokens = TOKEN_RE.findall(text)
    return bytes(int(t, 16) for t in tokens)


def fmt_hex(b: int) -> str:
    return f"0x{b:02X}"


@dataclass
class DiffResult:
    len_a: int
    len_b: int
    first_diff: int | None
    n_diffs: int


def diff_bytes(a: bytes, b: bytes) -> DiffResult:
    n = min(len(a), len(b))
    first = None
    n_diffs = 0
    for i in range(n):
        if a[i] != b[i]:
            n_diffs += 1
            if first is None:
                first = i
    # If lengths differ, count that as diffs too (but we keep first_diff as first differing index within overlap).
    n_diffs += abs(len(a) - len(b))
    return DiffResult(len(a), len(b), first, n_diffs)


def list_mismatch_offsets(a: bytes, b: bytes) -> list[int]:
    n = min(len(a), len(b))
    return [i for i in range(n) if a[i] != b[i]]


def summarize_offset_ranges(offsets: list[int]) -> str:
    if not offsets:
        return "(none)"
    ranges: list[tuple[int, int]] = []
    start = prev = offsets[0]
    for x in offsets[1:]:
        if x == prev + 1:
            prev = x
            continue
        ranges.append((start, prev))
        start = prev = x
    ranges.append((start, prev))
    parts = []
    for lo, hi in ranges:
        if lo == hi:
            parts.append(f"0x{lo:X}")
        else:
            parts.append(f"0x{lo:X}..0x{hi:X}")
    return ", ".join(parts)


def parse_packet_fields(buf: bytes) -> dict[str, tuple[int, int]]:
    """Best-effort field map for the Servomotor protocol.

    Format (TX):
      size_byte (1)
      [extended_size (2) if decoded_size==127]
      address: alias(1) OR (EXTENDED_ADDRESSING(1) + unique_id(8))
      command_id (1)
      payload (variable)
      crc32 (4) if enabled

    We can't know crc-enabled from sniff alone, but in this project it is typically enabled.
    """
    m: dict[str, tuple[int, int]] = {}
    if not buf:
        return m

    size0 = buf[0] >> 1
    extended = size0 == 127 and len(buf) >= 3
    off = 1
    if extended:
        m["size_bytes"] = (0, 3)
        off = 3
    else:
        m["size_bytes"] = (0, 1)
        off = 1

    if off >= len(buf):
        return m

    if buf[off] == 0xFE and off + 1 + 8 < len(buf):
        m["extended_addressing_byte"] = (off, off + 1)
        m["unique_id_le"] = (off + 1, off + 1 + 8)
        off = off + 1 + 8
    else:
        m["alias"] = (off, off + 1)
        off = off + 1

    if off < len(buf):
        m["command_id"] = (off, off + 1)
        off += 1

    # Assume CRC32 is present (most common in this codebase). If too short, treat as no-CRC.
    if len(buf) >= off + 4:
        m["payload"] = (off, len(buf) - 4)
        m["crc32_le"] = (len(buf) - 4, len(buf))
    else:
        m["payload"] = (off, len(buf))
    return m


def hexdump_slice(buf: bytes, start: int, end: int, cols: int = 16) -> str:
    out: list[str] = []
    start = max(0, start)
    end = min(len(buf), end)
    for row_off in range(start - (start % cols), end, cols):
        row = buf[row_off : row_off + cols]
        hexpart = " ".join(f"{b:02X}" for b in row)
        out.append(f"{row_off:08X}: {hexpart}")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare two sniff capture files byte-for-byte")
    ap.add_argument("file_a")
    ap.add_argument("file_b")
    ap.add_argument("--context", type=int, default=64, help="Context bytes around first mismatch")
    args = ap.parse_args()

    a = load_bytes(args.file_a)
    b = load_bytes(args.file_b)

    r = diff_bytes(a, b)
    print(f"A: {args.file_a} -> {r.len_a} bytes")
    print(f"B: {args.file_b} -> {r.len_b} bytes")

    if r.n_diffs == 0 and r.len_a == r.len_b:
        print("MATCH: buffers are identical")
        return 0

    if r.first_diff is None:
        # No mismatches within overlap; only length differs.
        print("DIFF: common prefix matches; lengths differ")
        if r.len_a > r.len_b:
            extra = a[r.len_b : r.len_b + min(64, r.len_a - r.len_b)]
            print(f"A has extra {r.len_a - r.len_b} bytes starting at offset {r.len_b}")
            print("A extra (first up to 64B):", " ".join(f"{x:02X}" for x in extra))
        else:
            extra = b[r.len_a : r.len_a + min(64, r.len_b - r.len_a)]
            print(f"B has extra {r.len_b - r.len_a} bytes starting at offset {r.len_a}")
            print("B extra (first up to 64B):", " ".join(f"{x:02X}" for x in extra))
        return 1

    i = r.first_diff
    assert i is not None
    print(f"DIFF: first mismatch at offset {i} (0x{i:X})")
    print(f"  A[{i}]={fmt_hex(a[i])}  B[{i}]={fmt_hex(b[i])}")

    mism = list_mismatch_offsets(a, b)
    print(f"DIFF: mismatches_in_overlap={len(mism)}")
    print("DIFF: mismatch ranges:")
    print("  " + summarize_offset_ranges(mism))

    # Best-effort interpretation: highlight if mismatches are confined to unique_id and/or CRC.
    fa = parse_packet_fields(a)
    fb = parse_packet_fields(b)
    # Use A's map (should match B structurally).
    def in_field(off: int, field: str) -> bool:
        if field not in fa:
            return False
        lo, hi = fa[field]
        return lo <= off < hi

    if mism:
        uniq = all(in_field(off, "unique_id_le") for off in mism)
        if uniq:
            print("NOTE: all mismatches are within unique_id_le (device unique ID differs)")
        else:
            uniq_crc = all(in_field(off, "unique_id_le") or in_field(off, "crc32_le") for off in mism)
            if uniq_crc and "crc32_le" in fa:
                print("NOTE: mismatches are confined to unique_id_le and crc32_le (CRC changes when unique ID differs)")

    # Show a small window around the mismatch
    lo = max(0, i - args.context)
    hi = i + args.context
    print("\nA hexdump:")
    print(hexdump_slice(a, lo, hi))
    print("\nB hexdump:")
    print(hexdump_slice(b, lo, hi))

    # Also report total number of mismatched positions within overlap
    n = min(len(a), len(b))
    mismatches = sum(1 for j in range(n) if a[j] != b[j])
    print(f"\nSummary: mismatches_in_overlap={mismatches}, len_a={len(a)}, len_b={len(b)}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
