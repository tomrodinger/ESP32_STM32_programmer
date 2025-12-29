#!/usr/bin/env python3

"""Plot sniffer gap data.

Reads `sniffer_timing_data.txt` written by [`tools/rs485_sniffer.py`](tools/rs485_sniffer.py:1)
and plots:
  x-axis: sample index
  y-axis: gap_ms

Outputs `sniffer_gaps.png` in the current directory.

Usage:
  python3 tools/plot_sniffer_gaps.py
  python3 tools/plot_sniffer_gaps.py --input sniffer_timing_data.txt --output sniffer_gaps.png
"""

from __future__ import annotations

import argparse


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot inter-byte gaps from sniffer timing data")
    ap.add_argument("--input", default="sniffer_timing_data.txt", help="Input data file")
    ap.add_argument("--output", default="sniffer_gaps.png", help="Output PNG path")
    ap.add_argument(
        "--dpi",
        type=int,
        default=600,
        help="PNG DPI (higher = more resolution). Default: 600",
    )
    ap.add_argument(
        "--interactive",
        action="store_true",
        help="Show an interactive/zoomable window instead of writing a PNG",
    )
    args = ap.parse_args()

    # Dependencies: matplotlib (not in repo requirements). Keep runtime error clear.
    try:
        import matplotlib.pyplot as plt  # type: ignore
    except Exception as e:
        print("ERROR: matplotlib is required for plotting.")
        print("Install into the existing venv:")
        print("  source .venv/bin/activate && pip install matplotlib")
        print(f"Import error: {e}")
        return 2

    # Try to enable interactive pan/zoom widget (matplotlib>=3.5 typically).
    try:
        from matplotlib.widgets import RectangleSelector  # type: ignore
    except Exception:
        RectangleSelector = None

    gaps_ms: list[float] = []
    with open(args.input, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("time_since_start_of_packet"):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            # Columns: time_since_start_of_packet_ms gap_ms max_gap_ms byte
            gaps_ms.append(float(parts[1]))

    if not gaps_ms:
        print(f"No data points found in {args.input}")
        return 1

    x = list(range(len(gaps_ms)))
    plt.figure(figsize=(16, 6))
    plt.plot(x, gaps_ms, linewidth=0.6)
    plt.title("Inter-byte gap (ms) vs sample index")
    plt.xlabel("Sample index")
    plt.ylabel("gap_ms")
    plt.grid(True, which="both", linestyle=":", linewidth=0.5)
    plt.tight_layout()

    if args.interactive:
        print("Showing interactive plot.")
        print("Tips:")
        print("  - Use the toolbar zoom/pan buttons")
        if RectangleSelector is not None:
            print("  - Drag-select to zoom (RectangleSelector enabled)")

        ax = plt.gca()

        if RectangleSelector is not None:
            def onselect(eclick, erelease):
                x1, y1 = eclick.xdata, eclick.ydata
                x2, y2 = erelease.xdata, erelease.ydata
                if None in (x1, y1, x2, y2):
                    return
                ax.set_xlim(min(x1, x2), max(x1, x2))
                ax.set_ylim(min(y1, y2), max(y1, y2))
                plt.draw()

            RectangleSelector(
                ax,
                onselect,
                useblit=True,
                button=[1],
                minspanx=5,
                minspany=0.1,
                spancoords="data",
                interactive=True,
            )

        plt.show()
        return 0

    plt.savefig(args.output, dpi=args.dpi)
    print(f"Wrote {args.output} (dpi={args.dpi})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
