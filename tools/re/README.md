# Randomizer discovery tools

Run from the repository root. These tools target the supported World XEX and
existing Release build. They do not install AP or change runtime hooks.

- `discovery-boot.ps1`: briefly boots with idle input and isolated user-data/cache,
  captures the already-supported `--dump_image`, then stops only its own child.
- `ap_discovery.py`: read-only strings, exact pointers, candidate lis/displacement
  references, direct call sites, generated-function disassembly and script-binding
  registration candidates. Binding inference is not a general PPC emulator; check
  each result against the displayed disassembly before relying on it.
- `discovery-live.ps1`: reaches New Game with the existing input replay diagnostic,
  samples bounded guest ranges using `live_snapshot.py`, then stops its own child.
  Each run has separate user-data/cache. `-RepeatMenu` tolerates cold-start timing.
  Optional `-Teleport x,y,z` uses the existing diagnostic after 12 seconds of
  gameplay and captures before/after samples; it changes only that isolated run.
- `live_snapshot.py`: opens the explicitly supplied PID with read/query rights,
  translates guest addresses using the arena base recorded in its log, and writes
  bounded samples/decoded observations. It never writes process memory or calls
  guest functions. Samples are not atomic across game threads.
- `export_discovery_catalog.py`: validates matching fresh-run orb metadata and boss
  IDs, plus supply-point/armory metadata when both snapshots have it, then exports
  metadata only. It does not assign public AP IDs.
- `validate_ap_probe.py`: verifies the orb/XP/Agility/safehouse integration captures
  documented in `docs/re-ap-probe.md`. This is an evidence validator, not a unit
  test that simulates gameplay. `discovery-live.ps1 -Probe`, `-NoXP` and
  `-AgilityLevel 2` exercise the opt-in in-process probe on isolated saves.
  `-Interact` sends Y after teleport; `-SixtyFPS` uses the normal 60 fps defaults.
  `-CaptureScreenshot` records only the spawned game's window.

All game-image/memory samples, logs, diagnostic saves and metadata exports live in
ignored `out/ap-discovery/`. Do not add the image or raw samples to git. Script
outputs need independent review; error-free reads alone do not prove a hook safe.

Examples:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/re/discovery-boot.ps1
python tools/re/ap_discovery.py strings 'Skill|Pickup|Boss|Spawn'
python tools/re/ap_discovery.py xrefs 820A4DB0 820C727C
python tools/re/ap_discovery.py function 82313968
python tools/re/ap_discovery.py bindings sub_823CAE70
python tools/re/ap_discovery.py callers 82543A28
powershell -NoProfile -ExecutionPolicy Bypass -File tools/re/discovery-live.ps1 -RunName live4 -RepeatMenu
python tools/re/export_discovery_catalog.py out/ap-discovery/live4-snapshot/snapshot.json out/ap-discovery/orb1-snapshot/snapshot.json
```

Capture scripts refuse an existing log/image instead of replacing evidence.
Choose a new `RunName` for another live capture. The original timed menu recording
is developer-local under `crackdown/bench/`; the repeating mode is provided for
machines where it is unavailable or timing differs. Native GUI/process sampling
may require execution approval in a sandbox.
