"""Write an --input_replay file for unattended AP runs.

Menu segment (0): A every 2 s, Start every 10 s, from 6 s (the same pattern as
discovery-live.ps1 -RepeatMenu); the replay hook drops the rest once gameplay
starts. Gameplay segment (1): optional button presses at given seconds.
Usage: ap_replay.py OUT [--press SECONDS:BUTTON ...] [--menu-until SECONDS]
       [--length SECONDS]   BUTTON is a, b, x, y, start, back.
"""
import argparse

BUTTONS = {"a": ("1000", "5800"), "b": ("2000", "5801"), "x": ("4000", "5802"),
           "y": ("8000", "5803"), "start": ("0010", "5814"), "back": ("0020", "5815"),
           "up": ("0001", "5810"), "down": ("0002", "5811")}


def press(seg, ms, name):
    button, key = BUTTONS[name]
    release = ms + 150
    return [f"S {seg} {ms} {button} 0 0 0 0 0 0", f"K {seg} {ms} {key} 0000 0001 0 0",
            f"S {seg} {release} 0000 0 0 0 0 0 0", f"K {seg} {release} {key} 0000 0002 0 0"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--press", action="append", default=[])
    ap.add_argument("--menu", action="append", default=[],
                    help="explicit menu presses SECONDS:BUTTON (replaces the A/Start pattern)")
    ap.add_argument("--menu-until", type=float, default=95)
    ap.add_argument("--length", type=float, default=180)
    args = ap.parse_args()
    lines = ["S 0 0.0 0000 0 0 0 0 0 0"]
    if args.menu:
        for spec in args.menu:
            sec, name = spec.split(":")
            lines += press(0, int(float(sec) * 1000), name)
    else:
        for ms in range(6000, int(args.menu_until * 1000), 2000):
            lines += press(0, ms, "start" if ms % 10000 == 0 else "a")
    lines.append("S 1 0.0 0000 0 0 0 0 0 0")
    for spec in args.press:
        sec, name = spec.split(":")
        lines += press(1, int(float(sec) * 1000), name)
    lines.append(f"S 1 {args.length * 1000:.1f} 0000 0 0 0 0 0 0")
    with open(args.out, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
