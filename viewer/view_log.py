#!/usr/bin/env python3

import csv
import sys
import subprocess

import plotly.graph_objects as go
import plotly.io as pio
from plotly.subplots import make_subplots


def load_events(path):
    events = []
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            # Be robust to blank/malformed lines
            if not row:
                continue
            t = row.get("t_ns")
            sig = row.get("signal")
            v = row.get("voltage")
            if t is None or sig is None or v is None:
                continue
            t = t.strip()
            sig = sig.strip()
            v = v.strip()
            if t == "" or sig == "" or v == "":
                continue
            events.append((int(t), sig, float(v)))
    return events


def build_step_series(events, signal, end_t):
    # Convert change-events into step-wise x/y arrays.
    xs = []
    ys = []

    last_t = None
    last_v = None

    for t, sig, v in events:
        if sig != signal:
            continue

        if last_t is None:
            xs.append(t)
            ys.append(v)
            last_t = t
            last_v = v
            continue

        if t == last_t:
            # Same timestamp: just update value
            ys[-1] = v
            last_v = v
            continue

        # Extend previous value up to t, then step to new value
        xs.append(t)
        ys.append(last_v)
        xs.append(t)
        ys.append(v)

        last_t = t
        last_v = v

    # Ensure each trace extends to the final timestamp in the log
    if last_t is not None and last_t < end_t:
        xs.append(end_t)
        ys.append(last_v)

    return xs, ys


WHEEL_JS = r"""
<script>
(function() {
  // Trackpad / wheel behavior:
  // - two-finger left/right (deltaX): pan horizontally
  // - two-finger up/down (deltaY): zoom horizontally
  // Vertical zoom is fixed by fixedrange=true on y axes.

  function attach(gd) {
    if (!gd) return;

    gd.addEventListener('wheel', function(e) {
      // Only operate when cursor is over the plot.
      e.preventDefault();

      const full = gd._fullLayout;
      const xa = full.xaxis;
      if (!xa || !xa.range) return;

      const x0 = xa.range[0];
      const x1 = xa.range[1];
      const span = (x1 - x0);
      if (!(span > 0)) return;

      const axisLen = xa._length || 1;
      const marginL = (full.margin && full.margin.l) ? full.margin.l : 0;

      // Decide whether to pan or zoom.
      const panPx = e.deltaX;
      const zoomDy = e.deltaY;

      // Prefer pan if horizontal scroll is dominant.
      if (Math.abs(panPx) > Math.abs(zoomDy)) {
        const dx = (panPx / axisLen) * span;
        Plotly.relayout(gd, {'xaxis.range': [x0 + dx, x1 + dx]});
        return;
      }

      // Zoom horizontally around cursor position.
      let px = e.offsetX - marginL;
      if (px < 0) px = 0;
      if (px > axisLen) px = axisLen;

      let center = xa.p2l ? xa.p2l(px) : (x0 + x1) / 2;
      if (!isFinite(center)) center = (x0 + x1) / 2;

      // Exponential zoom: trackpad feels smoother.
      // Positive deltaY -> zoom out; negative -> zoom in.
      const k = 0.0015;
      const factor = Math.exp(zoomDy * k);

      const newSpan = Math.max(50, span * factor); // clamp to >= 50ns
      const leftFrac = (center - x0) / span;
      const newX0 = center - leftFrac * newSpan;
      const newX1 = newX0 + newSpan;

      Plotly.relayout(gd, {'xaxis.range': [newX0, newX1]});
    }, {passive: false});
  }

  // Find the Plotly graph div(s) in this document.
  window.addEventListener('load', function() {
    const plots = document.querySelectorAll('div.js-plotly-plot');
    plots.forEach(attach);
  });
})();
</script>
"""


def main():
    if len(sys.argv) < 2:
        print("Usage: viewer/view_log.py signals.csv")
        return 2

    path = sys.argv[1]
    events = load_events(path)
    if not events:
        print(f"No events in {path}")
        return 2

    end_t = max(t for t, _, _ in events)

    # Determine available signals
    signals = sorted({sig for _, sig, _ in events})
    want = [s for s in ["NRST", "SWCLK", "SWDIO"] if s in signals]

    # Simulator step markers (logged as point-events with a constant voltage value).
    # We plot them on the SWDIO subplot so they are easy to find while panning.
    step_sigs = sorted([s for s in signals if s.startswith("STEP_")])

    fig = make_subplots(
        rows=len(want),
        cols=1,
        shared_xaxes=True,
        vertical_spacing=0.02,
        subplot_titles=want,
    )

    # Helper: collect point-events for a given signal.
    def collect_points(sig_name):
        xs = []
        ys = []
        for t, sig, v in events:
            if sig == sig_name:
                xs.append(t)
                ys.append(v)
        return xs, ys

    for i, sig in enumerate(want, start=1):
        xs, ys = build_step_series(events, sig, end_t)
        fig.add_trace(
            go.Scatter(
                x=xs,
                y=ys,
                mode="lines",
                name=sig,
                line=dict(width=2),
            ),
            row=i,
            col=1,
        )

        # Overlay SWDIO sampling markers on the SWDIO subplot.
        if sig == "SWDIO":
            if "SWDIO_SAMPLE_H" in signals:
                hx, hy = collect_points("SWDIO_SAMPLE_H")
                fig.add_trace(
                    go.Scatter(
                        x=hx,
                        y=hy,
                        mode="markers+text",
                        name="Host sample",
                        text=["H"] * len(hx),
                        textposition="middle center",
                        textfont=dict(size=10, color="white"),
                        marker=dict(
                            symbol="circle",
                            size=12,
                            color="#1f77b4",
                            line=dict(color="#1f77b4", width=1),
                        ),
                        hovertemplate="t=%{x} ns<br>Host sample<extra></extra>",
                    ),
                    row=i,
                    col=1,
                )

            if "SWDIO_SAMPLE_T" in signals:
                tx, ty = collect_points("SWDIO_SAMPLE_T")
                fig.add_trace(
                    go.Scatter(
                        x=tx,
                        y=ty,
                        mode="markers+text",
                        name="Target sample",
                        text=["T"] * len(tx),
                        textposition="middle center",
                        textfont=dict(size=10, color="white"),
                        marker=dict(
                            symbol="circle",
                            size=12,
                            color="#ff7f0e",
                            line=dict(color="#ff7f0e", width=1),
                        ),
                        hovertemplate="t=%{x} ns<br>Target sample<extra></extra>",
                    ),
                    row=i,
                    col=1,
                )

            # Overlay high-level step markers (STEP_*).
            if step_sigs:
                # Flatten into a single trace.
                sx = []
                sy = []
                st = []
                for t, sig2, v in events:
                    if sig2 in step_sigs:
                        sx.append(t)
                        sy.append(v)
                        st.append(sig2.replace("STEP_", ""))

                fig.add_trace(
                    go.Scatter(
                        x=sx,
                        y=sy,
                        mode="markers+text",
                        name="Steps",
                        text=st,
                        textposition="top center",
                        textfont=dict(size=10, color="#111"),
                        marker=dict(
                            symbol="triangle-up",
                            size=10,
                            color="#2ca02c",
                            line=dict(color="#2ca02c", width=1),
                        ),
                        hovertemplate="t=%{x} ns<br>%{text}<extra></extra>",
                    ),
                    row=i,
                    col=1,
                )

        fig.update_yaxes(title_text="V", row=i, col=1, range=[-0.2, 3.6], fixedrange=True)

    # Constrain interactions to X only.
    fig.update_layout(
        title=f"SWD Waveforms from {path}",
        height=250 * len(want) + 150,
        showlegend=True,
        hovermode="x unified",
        dragmode="pan",
    )

    fig.update_xaxes(title_text="time (ns)", row=len(want), col=1)
    fig.update_xaxes(matches="x")

    # Generate a self-contained HTML and open it.
    html = pio.to_html(
        fig,
        full_html=True,
        include_plotlyjs="cdn",
        config={
            # We implement our own wheel behavior.
            "scrollZoom": False,
            "displayModeBar": True,
        },
    )

    # Inject wheel handler.
    html = html.replace("</body>", WHEEL_JS + "</body>")

    out_path = "waveforms.html"
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)

    # Use macOS `open` but suppress any AppleScript/stdout noise.
    # If it fails, user can open the HTML file manually.
    try:
        subprocess.run(["open", out_path], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

    print(f"Wrote: {out_path} (open it in a browser if it did not auto-open)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
