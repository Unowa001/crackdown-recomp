# Randomizer discovery results — 2026-10-12

Author: Codex. Runtime hooks unchanged. Review and hook experiments remain pending.
Target XEX SHA256: `7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a`.

## Evidence and limits

The existing Windows Release executable was booted with separate user-data/cache
directories under `out/ap-discovery/`. Existing player profiles were not loaded.
The existing replay and teleport diagnostics drove the experiments; no AP hooks,
process-memory writes by the sampler, or guest-function injection were added.
Each run stopped only its own child process.

The image capture is `out/ap-discovery/guest-image.bin`, base `0x82000000`,
23,527,424 bytes, SHA256 `4788d2d74b2f671cc8e1095f5f31f91703aa4a539e5eb91ca754c14793493b11`.
It is a loaded image, not the XEX file. Generated disassembly is local under
`crackdown/generated/default`; no image, raw memory sample or generated source
is included in this change.

`live`, `live2`, `live4`, `orb1`, `hidden1`, `general1`, and `kingpin1` logs and snapshot
directories contain the successful captures. `live3` missed New Game with the
original timed menu replay and timed out; repeating menu input then succeeded.
All observations below distinguish static interpretation from measured state.
No Xenia comparison, boss kill, save/reload reconciliation, XP suppression or
native skill-grant experiment has yet been performed. Reads across threads are
not atomic; the compared samples were taken during idle gameplay.

## Orb catalog and collection — measured

The live pickup manager contains **800 entries: 500 agility and 300 hidden**.
Its global pointer is at guest `0x82DE3F34`. Constructor `sub_824EEEE8` and
allocator `sub_824EEE80` establish a 14,040-byte manager.

| Manager offset | Observed/interpreted layout |
|---|---|
| +12 | Fixed array of up to 1,000 pickup pointers |
| +4012 | Number of loaded entries (800) |
| +4016 | Hidden-orb total (300) |
| +4020 | Agility-orb total (500) |
| +4040 | 50 × 50 spatial bucket heads; query follows pickup +260 links |

The primary orb object is registered as class ID 274, 320 bytes
(`sub_824EFCF0`), not `cStarPickup` (a separate class, ID 268).

| Pickup offset | Meaning/evidence |
|---|---|
| +260 | Next pickup in spatial bucket (`sub_824EF6F8`) |
| +264 | Type: 0–3 agility variants; 4 hidden |
| +268 | Zero in all 800 sampled entries; **not an ID** |
| +272/+276/+280 | Position floats (x/y/z) |
| +288/+289 | Availability for player indices 0/1; 1 available, 0 collected |
| +292 | Runtime visual/effect reference; not a location identity |

Type counts were 231, 145, 101, 23 and 300 respectively. Two independent fresh
games (`live4-snapshot` and `orb1-snapshot`) had exactly matching array order,
types and positions despite different object addresses. Candidate location identity
is the **save-array ordinal**, not the pointer. Native save/load callbacks
`sub_824EF1E0`/`sub_824EF158` iterate the same array and serialize each availability
flag as a 4-byte word. This is not the originally guessed packed bitfield.
Confirm ordinal stability through save/reload and resource-order variations before
assigning permanent AP IDs; bind the catalog/version to the supported XEX.

`sub_82313968` is the collection path. It queries the pickup manager with
`sub_824EF6F8` (on-foot) or `sub_824EF588` (alternate/vehicle path), clears
`pickup[288 + player_index]`, increments counters, displays the pickup HUD text,
emits events, and queues the XP reward with `sub_82547B40`.
The agility counter is at primary-player +4396; hidden at +4392.
Trigger enum `sub_82B3C090` names event 116 as `eTriggerEvent_AgilityPickup`
and 117 as `eTriggerEvent_HiddenOrbPickup`; `sub_8258D0F0` dispatches them.

### Actual agility pickup

Experiment `orb1`: fresh isolated game, idle input, teleport after 12 seconds to
`1394.076,31.303,-1408.959`. Before/after snapshots show:

- Only array entry **178**, type 0, changed availability `[1,1] → [0,1]`.
- Its authored position is `1394.075928,32.302891,-1408.958862`.
- Primary-player agility count changed 0 → 1; hidden count stayed 0.
- Skill index 0 XP at SkillSet +28 changed 0 → 10; other skills stayed unchanged.
- The raw level bytes stayed at 1.

This validates a real collectible/check identity and the XP bank, without altering
vanilla rewards. Suppressing XP while retaining the native collection effects is
the next hook experiment, not something already demonstrated.

### Actual hidden pickup

Experiment `hidden1`: teleport to `1469.650,15.750,-1460.849` in another fresh
isolated game. Only ordinal **508**, type 4, changed availability `[1,1] → [0,1]`.
Hidden count changed 0 → 1; agility count stayed 0. It awarded 10 XP to each skill:
agility converted its bonus chunk into ordinary bank +28 =10, while driving,
explosives, strength and firearms retained +48 =10 below their conversion thresholds.
All level bytes stayed 1. This independently validates the hidden branch and why
both XP banks matter. Loaded reward data at RPG word indices 192–195 is
10/20/50/100 for the four agility variants; indices 197–201 are 10 each for hidden
rewards. Only the type-0 agility reward was observed live in this pass.

`out/ap-discovery/catalog.json` is a complete metadata-only export of all 800
positions/types/ordinal candidates, 21 boss IDs/names and skill tables, with source
snapshot hashes. Its schema is discovery-v0; AP IDs are deliberately unassigned.

## Skills — measured fields, static grant path

Global `0x82DE3F3C` points to the primary player object. Its component at +292,
minus 32, yields the character. In the measured character vtable `0x8208D850`,
slot +1220 points to `sub_82299350`, which returns **character +9056**, the SkillSet.
Do not hardcode the sampled heap addresses.

| SkillSet offset | Layout/interpretation |
|---|---|
| +8 | Owner character pointer |
| +12..16 | Five base/effective-level bytes read by the native getter |
| +17..21 | Five progression-level bytes used by XP and setter paths |
| +22..26 | Five bonus bytes; getter adds max(bonus−1,0) |
| +28 +4×index | Five ordinary XP float banks |
| +48 +4×index | Five bonus/queued-XP float banks |
| +340..343 | Dirty/notification/mode bytes; meanings not fully validated |

Indices from the existing debug-console registrations and their actual arguments:
0 agility/athletics, 1 driving, 2 explosives, 3 strength/fighting, 4 firearms.
Fresh-game samples had both level arrays at 1, all bonus bytes zero, and all XP zero.

`sub_82544950` gets the effective native level, clamped 1–5.
`sub_82543DB0` sets the progression level (r3 SkillSet, r4 index, r5 raw level,
r6 notification/control argument), clamps 1–5, resets both XP banks, marks dirty
state and calls `sub_82543E70` for character/HUD/skill side effects. Four upgrades
from raw level 1 to 5 are consistent with the proposed Progressive items; verify
the HUD/star mapping and base/effective-level synchronization with a real grant.
Directly writing one level byte is not an adequate grant implementation.

`sub_82543A28` adds bonus XP, promotes accumulated bonus chunks through
`sub_82543BB0`, and handles capped states. `sub_82543BB0` adds ordinary XP,
subtracts per-level costs when leveling and updates level/notification state.
`sub_82547DC8` consumes the animated XP pickups and calls the bonus-XP routine;
`sub_82547B40` can award directly when its pickup pool is full. Suppressing only
the visual pickup-consume path would miss that overflow reward path.

The RPG configuration pointer is at `0x82D990AC`. Measured table lookup uses
word index `136 + 5×skill + raw_level` for bonus conversion and
`161 + 5×skill + raw_level` for ordinary XP costs. For raw levels 1–5:

| Skill | Bonus-conversion values | Ordinary XP costs |
|---|---|---|
| Agility | 10, 50, 200, 400, 650 | 100, 500, 2000, 4000, 6500 |
| Driving | 30, 30, 30, 30, 30 | 350, 700, 1750, 3500, 7000 |
| Explosives | 100, 100, 100, 100, 100 | 1000, 2000, 5000, 10000, 20000 |
| Strength | 30, 30, 30, 30, 30 | 300, 600, 1500, 3000, 6000 |
| Firearms | 100, 100, 100, 100, 100 | 1000, 2000, 5000, 10000, 20000 |

These are native relative-bank costs, not established cumulative XP floors.
The original plan's "set XP to the level's floor" needs adjustment to the native
setter/bank behavior. Capped filler must account for both banks and pending
animated XP, preserve received levels and never accidentally trigger a promotion.

`AgentSkills.sav` is referenced at `0x820C72B4` by `sub_825431D8` and
`sub_82503340`. SkillSet load/save routines `sub_82543700`/`sub_825437E0` serialize
mode, both level arrays and both XP banks. Save/profile binding still needs proof.

## Boss catalog, defeat and gating

Global `0x82DE27F4` points to the 304-byte gang manager. `sub_823C6728`
computes the boss-vector count; pointer array starts at manager +208 and its
end pointer is at +192. `sub_823CD490` indexes boss info by ID.
Live games repeatedly contain **21 bosses with IDs 0–20**.

| ID | Name | ID | Name |
|---|---|---|---|
| 0 | Rodrigo Alvarez | 11 | Vladimir Golyak |
| 1 | Violetta Sanchez | 12 | Olga Drankov |
| 2 | Juan Martinez | 13 | Viktor Rabotnikov |
| 3 | Don Domingo Garcia | 14 | Mrs Timbol |
| 4 | Jose Guerra | 15 | Baltazar Czernenko |
| 5 | Ramon Gonzalez | 16 | Melissa Fang Yin |
| 6 | Rafael Diaz | 17 | Zhuang Lun Wang |
| 7 | Natalya Gryzunova | 18 | Colonel Axton Powell |
| 8 | Igor Biragov | 19 | Thadeous Oakley |
| 9 | Boris Mikhailov | 20 | Vitaly Rzeznik |
| 10 | Sergei Sovetnik | | |

The expected kingpin goal identities are Garcia/Golyak/Wang (IDs 3/11/17);
their special mission/role flags have not been independently validated.

`cBossInfo` class ID 1036 is 176 bytes (`sub_823CADC8`). Key fields:
ID +32 (`sub_8215B6A0`), name string +36, gang pointer +64, mission difficulty
+124 (`sub_823C7D10`), position +128, intro-played byte +145
(`sub_823C7D20`), dead byte +148, spawn descriptor +156, actor +160.
`sub_824F2E38` reads the dead byte with game-mode overrides.
`sub_823CBA98` is the registered `OnKilled` callback. It sets the dead byte and
touches gang/world/save/achievement state; do not skip its whole body for AP.

### Shared boss-specific spawn/eligibility chain

`sub_823CB2A8` constructs each boss's 1,256-byte spawn descriptor with
`sub_823EA658` and stores it at boss +156. The descriptor has vtable `0x820A8318`,
and a back-pointer to its boss at +884.

Its state object starts at descriptor +872 (getter `sub_823E8D10`) with vtable
`0x820A83C0`. State vtable +4 is **`sub_823E9248`**, a boolean eligibility
predicate. It reads the boss through state +12, consults the boss dead/mission
state and player proximity, and returns a boolean. Descriptor vtable +40,
`sub_823EAB18`, calls base activation `sub_823E9C30`, which consults that predicate
and already has an unavailable/false return path.

This is the strongest gate candidate: in randomized mode, require the matching
access item in `sub_823E9248` before allowing the original predicate. Preserve
its existing dead, mode, proximity and mission rules. This would target the
boss-specific lifecycle rather than all NPC spawning. Validate retry/unlock
behavior before implementation is declared safe; no access-gate hook was installed.

`sub_82150D48` (`SetBossInfo`) associates a spawned character: character +3176
gets boss info, boss +160 gets the character, and descriptor +888 gets an associated
controller reference. It is an observation point, not the preferred pre-spawn gate.

### General and kingpin observations

- `general1`, Rodrigo (ID 0), teleport to `410.36,4.81,-1381.25`:
  boss +160 changed **0 → 0x6F649CA0**; dead stayed 0.
- `kingpin1`, Garcia (ID 3), teleport to `286.10,58.66,-2406.53`:
  boss +160 changed **0 → 0x6F630020**; dead stayed 0.
- Both used descriptor vtable `0x820A8318` and state getter `sub_823E8D10`.
- Descriptor byte +876 stayed zero in the general experiment despite spawning.
  It is **not** a demonstrated simple spawn-enable switch. Do not flip it blindly.

`CanSpawnInstance` (`sub_821F93D8`) is an ambient-crime script-VM call, not a
confirmed boss gate. Do not replace it globally based on its name.

## Next controlled implementation experiments

1. One orb check: map the collected pickup to its catalog ordinal, preserve native
   collection/counters, suppress its XP, and show a unique check event. Repeat
   pickup/reload tests; verify hidden and alternate/vehicle paths.
2. One Progressive Agility grant: call the native setter on the game thread,
   confirm HUD/movement and both level arrays, then save/reload. Audit all XP
   sources and pending pickup pools before enforcing the capped-filler rule.
3. One general and one kingpin gate: interpose the boss-specific eligibility
   predicate, require an explicit key, and test closed/open while near, far,
   reloading, dead and already defeated. Keep all kill/weakening effects native.
4. Save identity and received-inventory recovery: finish these before connecting
   the proof to an AP server or promising durable progression.

Use separate reviewed `wip/ap-*` implementation changes. The memory samples and
metadata establish discovery results, not a completed randomizer or gate system.
