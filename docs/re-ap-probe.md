# First in-process randomizer contract probe

2026-10-12, `wip/ap-probe`, authored and exercised by Codex. Claude Code review
and merge remain pending. Supported XEX SHA256:
`7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a`.

`crackdown/src/ap/ap_probe.cpp` is an opt-in diagnostic slice. It does not connect
to Archipelago, assign public location IDs, bind a seed/save, or persist checks.
Its log observations must not be presented as checks already sent to a server.
All test saves/cache and screenshots are local under ignored `out/ap-discovery/`.
Existing player saves were not loaded. All flags default off.

## Runtime paths and disassembly

Read with `python tools/re/ap_discovery.py function ADDRESS` from the supported
generated functions. Initial field provenance is in
[the discovery report](re-discovery-randomizer.md).

| Address | Disassembly evidence | Probe behavior |
|---|---|---|
| `82313968` | `82313D38`: selected pickup in r27; `add r11,r27,r28; stb r25,288(r11)` clears the collecting player's availability; subsequent branches update counters and queue XP | Wrap the original body; log only observed 1-to-0 availability changes by catalog ordinal/player |
| `82543A28` | r3 SkillSet, r4 skill index, f1 amount; stores bonus XP at `+48+4*index`; calls `82543BB0` on conversion | Optional early return for the verified primary-agent SkillSet only |
| `82543BB0` | r3 SkillSet, r4 skill index, f1 amount; ordinary bank at `+28+4*index`; threshold loop increments `+17+index` | Same primary-agent suppression for direct ordinary XP |
| `82543DB0` (called, not replaced) | r5 raw level clamped 1-5; writes progression, clears both XP banks, marks dirty and calls `82543E70` | Optional one-time Agility grant after a collected orb |
| `823911A0` | r4 player, r5 supply index; byte address `manager+2616+40*player+index`; writes 1; r3 returns whether it was previously zero | Log native first-unlock transitions with caller LR; no interception of availability |
| `82642760` | `826427EC` onward enumerates carried inventory, resolves each armory entry through `82644A50`; byte `entry+9` changes 0-to-1; newly deposited entries are appended to the caller's vector | Wrap primary-player deposit scan and log native flag transitions |

The native setter/control argument follows console callback `825087A8`:
`li r6,1; extsb r5,r11; mr r4,r29; bl 82543DB0`. A copied PPC context preserves
the wrapped function's returned registers while the native setter uses the same
game-thread guest stack. The observed SkillSet is `character+9056`, owner at `+8`;
require character vtable `8208D850` and getter slot `+1220 == 82299350` before
suppressing XP or granting. This is a supported-build diagnostic guard, not a
general solution to save/loading lifetime safety.

Orb snapshots scan 800 entries around the original collection call. That deliberate
diagnostic overhead is disabled in ordinary play. Production event hooks should
capture the actual selected pickup and verify its stable ID without a full scan
per collection poll. No second frame-loop hook was added.

## Validation recorded locally

Release built successfully with LLVM/VS x64. Each run used a fresh user-data/cache
root. Before/after samples have no read errors. Image/window captures use
`PrintWindow` on the spawned child's window, not a desktop screenshot.

| Run | Result | Screenshot comparison |
|---|---|---|
| `probe-orb-vanilla`, stock 30 fps, observation on | Only orb 178/type 0/player 0 collected; exactly one observation; agility counter 1; ordinary Agility XP 10 | `probe-orb-vanilla-window.png`: vanilla HUD Agility XP 10 |
| `probe-orb-no-xp`, stock 30 fps, suppression + explicit Agility 2 | Same one-orb transition; one setter call; base/progression both 2; all five skills' XP banks 0 | `probe-orb-no-xp-window.png`: two Agility stars, XP 00, "AGILITY SKILL INCREASED" |
| `probe-hidden-no-xp`, stock 30 fps, suppression without grant | Only orb 508/type 4/player 0 collected; hidden counter 1; five 2-XP bonus awards suppressed per skill (10 each); all levels 1, both banks 0 | `probe-hidden-no-xp-window.png`: no skill increase, HUD XP 00 |
| `probe-disabled60`, default 60 fps, all probe flags off | Same orb 178 collected; ordinary XP 10; no probe log entries | `probe-disabled60-window.png`: normal XP 10 |
| `probe-supply0-enter`, stock 30 fps, observation on | Supply 0 player 0 unlocks once, caller `82533490`; only its unlock flags change `[0,0]` to `[1,0]`; starting armory unchanged | `probe-supply0-enter-window.png`: supply-point loadout UI |

`python tools/re/validate_ap_probe.py` checks those recorded transitions, event
counts, XP/level fields, safehouse capture and matching fresh-run orb catalogs. It passes. Suppression
of the ordinary award function is implemented from disassembly; these pickup runs
exercise bonus awards, so a separate direct-ordinary award test remains necessary.
Movement/jump height, save/load persistence and all five native setters are not yet
validated by this slice. A grant clearing XP could mask an earlier award; the
separate hidden-orb run therefore validates suppression without any grant.

Commands to reproduce (use fresh RunNames and update the validator inputs if the
existing evidence directories are retained):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/re/discovery-live.ps1 -RunName probe-orb-vanilla -RepeatMenu -Probe -Teleport '1394.076,31.303,-1408.959' -CaptureScreenshot
powershell -NoProfile -ExecutionPolicy Bypass -File tools/re/discovery-live.ps1 -RunName probe-orb-no-xp -RepeatMenu -NoXP -AgilityLevel 2 -Teleport '1394.076,31.303,-1408.959' -CaptureScreenshot
powershell -NoProfile -ExecutionPolicy Bypass -File tools/re/discovery-live.ps1 -RunName probe-hidden-no-xp -RepeatMenu -NoXP -Teleport '1469.650,15.750,-1460.849' -CaptureScreenshot
powershell -NoProfile -ExecutionPolicy Bypass -File tools/re/discovery-live.ps1 -RunName probe-disabled60 -RepeatMenu -SixtyFPS -Teleport '1394.076,31.303,-1408.959' -CaptureScreenshot
powershell -NoProfile -ExecutionPolicy Bypass -File tools/re/discovery-live.ps1 -RunName probe-supply0-enter -RepeatMenu -Probe -Teleport '1304.342,43.357,-1476.225' -Interact -AfterTeleportSeconds 12 -CaptureScreenshot
python tools/re/validate_ap_probe.py
```

## Safehouse and weapon contract discovery

Supply manager global `82DE2678`, allocated as 2816 bytes by `82322F28` and
constructed by `82390CD8`; reset `82390E10`. Capacity 40; actual count `+8 = 16`.
Transforms start `+16`, stride 64; translation is `+64+64*index`. Per-point required
level candidate `+2576+index`, unlock bytes `+2616+40*player+index`, visited bytes
`+2712+2*index+player`. `82391118` updates the visited/current-point state separately
from `823911A0` unlocking. Fresh game entry 15 is already unlocked for both players;
entries 0-14 are locked. Do not award a securing check from that initial owned flag.

`82533430` is the entry path: nearest point `82390EF0`, mark visit `82391118`, unlock
`823911A0`, then weapon-deposit/UI `82642950`; first capture dispatches event 35.
Another direct unlock caller is UI path `8263E2D0`; LR/source and save/load boundaries
must be understood before all unlock calls can become gameplay checks. Debug
`unlock_supply_points` callback `825092A0` only sets bypass byte `82DE262F`; it is
not the legitimate per-point capture event.

Supply-point 0 teleport at y=38.357 landed beneath the platform (`probe-supply0`).
At y=43.357 the agent lands at the orange entry pad (`probe-supply0-high-window.png`).
The HUD shows **Y: Enter Supply Point**. The earlier X input did not capture it;
`-Interact` now sends Y. These failed interactions produced no unlock observation.
The corrected `probe-supply0-enter` run unlocks supply 0 (Pacific City Bank, named
by the loadout UI) for player 0 once. LR is `82533490`, immediately after the capture path's call at
`8253348C`. Agency starting ownership produces no unlock event. Entering with
already-available starting equipment changes no armory flags and emits no weapon
deposit transition. A locked weapon's first deposit still needs a gameplay run.

Armory singleton pointer `835D3F84`, getter `8250E0D8`, constructor `82645460`.
Vector begin/end at manager `+12/+16`, 112-byte entries. Native ID `+12`, category
`+16` (1 firearm, 2 throwable/explosive), starting available `+8`, mutable armory
availability `+9`, wide lookup name at `+56` (inline characters `+60` or pointer,
capacity `+80`). `82644A50` matches inventory weapon definition type/name and returns
the armory record. Fresh snapshots consistently enumerate IDs 1-19:

| ID | Native name | Initially available |
|---|---|---|
| 1 | Colby EAR50 Machine Gun | yes |
| 2 | Colby Enforcer Pistol | yes |
| 3 | Colby Limpet Charge | yes |
| 4 | Smithson 'Punisher' | no |
| 5 | Ishkov 'Diktat' | no |
| 6 | Harman MP-50 | no |
| 7 | Ankram X80 SMG | no |
| 8 | Ankram MG-60 | no |
| 9 | Harlington HMG-90 | no |
| 10 | Dempsey 190 'Equalizer' | no |
| 11 | Dempsey SO-6 'Stub' | no |
| 12 | Bastion S600 'Hawkeye' | no |
| 13 | Bastion SX900 'Longshot' | no |
| 14 | Watson HE79 'Grenadier' | no |
| 15 | Watson HE99 'Hotshot' | no |
| 16 | Watson HE99-X 'Firefly' | no |
| 17 | Shrapnel Grenade | no |
| 18 | Grenade | yes |
| 19 | Cluster Grenade | no |

The owner requires weapon and safehouse checks in the first full release. A
production weapon check cannot simply watch armory `+9`: AP receipt can set it
before the player discovers/deposits the weapon. Track natural discovery separately
from AP grants and rack equips, and handle starting weapons explicitly. The probe
logs native deposit transitions only; it does not solve that distinction yet.
Receiving teleport access likewise cannot emit a securing check. Agency initial
ownership and a gameplay visit must be distinguished.

Metadata export `out/ap-discovery/catalog-probe.json` now contains the 800 orbs,
21 bosses, 16 supply entries and 19 armory entries, verified across separate fresh
snapshots. No public AP IDs are assigned. Reload stability, actual first deposit
of a previously unavailable weapon and received-unlock separation remain pending.

## Next implementation acceptance

Finish natural weapon acquisition/deposit and supply capture provenance, then
save/load ID and session lifetime validation. Exercise direct ordinary XP awards,
all native skill setters and capped filler. Implement durable observed checks and
received inventory separately before connecting the in-process AP client. Boss
gating retains its own general/kingpin activation spike; no gate is installed here.
