"""Summarize --diag_audio logs; join clip identity across reused voice addresses.

Usage: python tools/diag/audio_lifetimes.py <log> [<log> ...]
Buckets are milliseconds: <500, <1000, <2000, <4000, <8000, >=8000.
Sibling rotated files (name.1.log, name.2.log, ...) are read oldest first.
"""

import bisect
import collections
import pathlib
import re
import sys


def summarize(path):
    buckets = [0] * 6
    clips = {}
    stopped = {}
    speech = []
    timed_out = 0
    banks = collections.Counter()
    last_stop = None
    finished = False
    older = sorted(path.parent.glob(path.stem + ".[0-9]*.log"),
                   key=lambda p: int(p.stem.rsplit(".", 1)[1]), reverse=True)
    for part in [*older, path]:
        with part.open(encoding="utf-8", errors="replace") as log:
            lines = list(log)
        for line in lines:
            finished |= "Title terminated; hard-exiting process" in line
            match = re.search(
                r"game_start instance=(\w+).*?name=(\S+) index=(\d+) "
                r"samples=(\d+) length_ms=([\d.]+) loop=(\d+).*?"
                r"voice=(\w+) deadline=(\d+)", line
            )
            if match:
                instance, bank, index, samples, length, loop, voice, deadline = match.groups()
                clips[voice] = (instance, bank, index, samples, length, loop, deadline)
                stopped.pop(instance, None)
            match = re.search(
                r"game_stop instance=(\w+).*?elapsed=\d+->(\d+) deadline=(\d+)", line
            )
            if match:
                instance, elapsed, deadline = match.groups()
                stopped[instance] = int(elapsed) > int(deadline)
            match = re.search(r"diag_audio: stop voice=(\w+) life_ms=(-?\d+)", line)
            if match:
                voice, lifetime = match.groups()
                lifetime = int(lifetime)
                last_stop = (voice, lifetime)
                if lifetime >= 0:
                    buckets[bisect.bisect_right([500, 1000, 2000, 4000, 8000], lifetime)] += 1
                clip = clips.pop(voice, None)
                if clip:
                    instance, bank, index, samples, length, loop, deadline = clip
                    timeout = stopped.pop(instance, False)
                    timed_out += timeout
                    banks[bank] += 1
                    if bank.startswith("VOA"):
                        speech.append((voice, bank, index, length, lifetime, timeout))
            # The final record follows stop immediately and contains the same
            # clip identity, so address reuse cannot associate the wrong clip.
            match = re.search(r"diag_audio: final voice=(\w+).*?name=(VOA\S+).*?context=\w+ "
                              r"([0-9A-F]{8})(?: [0-9A-F]{8}){8} ([0-9A-F]{8})", line)
            if match and last_stop and match[1] == last_stop[0]:
                word0, word9 = int(match[3], 16), int(match[4], 16)
                valid = (word0 >> 20) & 3
                write, read = (word0 >> 27) & 31, word9 & 31
                print(f"  XMA voice={match[1]} input_valid={valid} output_write/read={write}/{read}")
    print(f"{path}: buckets={buckets} voices={sum(buckets)} "
          f"timeout_stops={timed_out} terminated={finished}")
    print("  banks:", dict(banks))
    for voice, bank, index, length, lifetime, timeout in speech:
        print(f"  {bank}[{index}] voice={voice} length_ms={length} "
              f"life_ms={lifetime} timeout={timeout}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for filename in sys.argv[1:]:
        summarize(pathlib.Path(filename))
