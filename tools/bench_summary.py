#!/usr/bin/env python3
"""Summarize one tools/bench.ps1 phase: guest frame times from the game's
--perf_log_csv and GPU time from PresentMon.

Usage: bench_summary.py <bench dir> <name>

PresentMon rows are host presents, which run on the presenter's cadence, not
once per guest frame. So GPU cost is reported as busy time per second of
capture divided by the guest frame rate: GPU ms per guest frame, comparable
with the frame budget (33.3 ms at 30 fps, 16.7 at 60, 8.3 at 120).
"""
import csv
import os
import subprocess
import sys


def presentmon(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    rows = [r for r in rows if r.get("MsGPUBusy") not in (None, "", "NA")]
    if len(rows) < 2:
        return None
    t0 = float(rows[0]["TimeInMs"])
    t1 = float(rows[-1]["TimeInMs"])
    span_s = (t1 - t0) / 1000.0
    busy = sum(float(r["MsGPUBusy"]) for r in rows)
    return len(rows), span_s, busy


def main():
    out, name = sys.argv[1], sys.argv[2]
    game_csv = os.path.join(out, name + ".csv")
    pm_csv = os.path.join(out, "pm_" + name + ".csv")
    here = os.path.dirname(os.path.abspath(__file__))

    print("== " + name)
    frames = subprocess.run(
        [sys.executable, os.path.join(here, "frametimes.py"), game_csv, "25", "1"],
        capture_output=True, text=True)
    line = (frames.stdout or frames.stderr).strip().splitlines()
    print("guest: " + (line[-1] if line else "no data"))

    pm = presentmon(pm_csv) if os.path.exists(pm_csv) else None
    if not pm:
        print("gpu:   no PresentMon data")
        return
    n, span_s, busy = pm
    try:
        fps = float(line[-1].split("(")[1].split(" fps")[0])
    except (IndexError, ValueError):
        fps = None  # no guest frame rate (e.g. a Release exe writes no CSV rows)
    per_s = busy / span_s
    if fps:
        print("gpu:   %d host presents over %.1f s, busy %.0f ms/s (%.0f%%), %.2f ms per guest frame at %.1f fps"
              % (n, span_s, per_s, per_s / 10.0, per_s / fps, fps))
    else:
        print("gpu:   %d host presents over %.1f s, busy %.0f ms/s (%.0f%%); guest fps unknown"
              % (n, span_s, per_s, per_s / 10.0))


if __name__ == "__main__":
    main()
