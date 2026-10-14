#!/usr/bin/env python3
"""Benchmark numbers from a --perf_log_csv file (playbook section 8).

Usage: frametimes.py perf.csv [seconds=30] [tail_trim_s=2]

One run per phase: hold the phase for at least seconds + trim + 1, then quit
at once. The window is the `seconds` before the last `tail_trim_s` of frames,
by cumulative frame time. 1% low = mean of the slowest max(1, n // 100)
frames, in ms.
"""
import csv
import sys


def main():
    path = sys.argv[1]
    seg_us = int(float(sys.argv[2] if len(sys.argv) > 2 else 30) * 1e6)
    trim_us = int(float(sys.argv[3] if len(sys.argv) > 3 else 2) * 1e6)
    if seg_us <= 0 or trim_us < 0:
        sys.exit("seconds must be > 0 and tail_trim_s >= 0")
    with open(path, newline="") as f:
        # The first row is 0 (no previous swap yet).
        # Window close can cut the last row short (host_tuning.cpp
        # FlushPerfCsvLog): keep only complete rows.
        rows = [r for r in csv.DictReader(f)
                if None not in r.values() and all(v.strip() for v in r.values())]
        ft = [v for v in (int(r["frame_time_us"]) for r in rows) if v > 0]

    # Walk back from the end: skip trim_us, then take frames until seg_us.
    end, t = len(ft), 0
    while end > 0 and t < trim_us:
        end -= 1
        t += ft[end]
    start, t = end, 0
    while start > 0 and t < seg_us:
        start -= 1
        t += ft[start]
    if t < seg_us:
        sys.exit(f"run too short: only {t / 1e6:.1f} s of frames before the trim")

    win = sorted(x / 1000 for x in ft[start:end])
    worst = win[-max(1, len(win) // 100):]
    avg = sum(win) / len(win)
    print(f"{len(win)} frames over {t / 1e6:.1f} s  avg {avg:.2f} ms ({1000 / avg:.1f} fps)"
          f"  1% low {sum(worst) / len(worst):.2f} ms  max {win[-1]:.2f} ms")


if __name__ == "__main__":
    main()
