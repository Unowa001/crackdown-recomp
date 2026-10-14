# Plan: Crackdown Archipelago randomizer

Current owner decisions and implementation acceptance are in
[randomizer-scope.md](randomizer-scope.md). Weapons first deposited into the armory
and secured safehouses/supply points are required check groups for the first full
release. Counts and hook evidence supersede the estimates below; see
[the runtime probe](re-ap-probe.md) and [initial discovery](re-discovery-randomizer.md).

Status: design plan, 2026-10-12, from the owner's brief (start in the Agency tower, everything unlocked; weapons, orbs,
generals and kingpins as checks; XP turned into items) plus brainstorm. Owner decisions are marked **[decide]**. The
world side follows the APWorldMaker toolkit (a separate APWorldMaker checkout, its docs/PLAN.md; ME1 is the worked
example in `games/masseffect1`). Order: after the beta launcher (roadmap). Phase 0 (RE) can start earlier because it is
read-only.

## 1. Why this is easier than a usual AP game

We own the executable. The AP client, check detection and item grants all live **inside `crackdown.exe`** as recomp
hooks (`crackdown/src/ap/`). No memory-poking sidecar, no address tables per build (one supported XEX, see the
launcher plan), no file-drop mailbox. In APWorldMaker axis terms: runtime topology = in-process; check protocol = native
hooks; item delivery = direct state write; dependency form = none for the user (the client ships in the exe).

## 2. Game rules for a randomized slot

- New game starts in the Agency tower, as vanilla. All islands/bridges open (vanilla already allows it; if any
  gating turns up in Phase 0, force it open).
- **Skill XP from gameplay is disabled** (hook the XP award function; option `skill_mode`, below). Skills rise only
  from received items.
- Picking up an orb, killing a boss, first-collecting a weapon, etc. **sends** a check and does not give the vanilla
  reward (no XP from orbs; the boss kill still counts for gang weakening, which is vanilla world state we leave alone).
- Supply-point weapon racks only offer weapons the slot has **received**.
- Agency vehicle tiers follow the received driving level (vanilla behaviour, since driving level is now an item).

## 3. Locations (checks)

Counts below are from general game knowledge; Phase 0 must replace them with the exact tables from game data.

| Group | Approx. count | Detection hook (to find in Phase 0) | Option |
|---|---|---|---|
| Agility orbs | 500 | orb-collected function / save bitfield | `agility_orbs`: all / every Nth / none |
| Hidden orbs | 300 | same family | `hidden_orbs`: all / none |
| Gang bosses (generals + 3 kingpins) | ~21 | boss-killed event (target list in data) | always on |
| Weapons first collected (into the armory) | ~15-20 | armory unlock | always on |
| Supply points secured | ~15 | supply-point state | `supply_points` |
| Rooftop races | ~14 | race completion | `races` |
| Road races (by car type) | ~?? | race completion | `races` |
| Stunt jumps / stunt rings | ~?? | stunt-marker completion | `stunts` |
| Skill level reached | 5 skills x 4 | only meaningful when `skill_mode = vanilla` | (off by default) |
| Achievements | ~?? | the game's achievement unlock calls (XamUserWriteAchievements in the kernel layer is an easy hook) | `achievement_checks` |

Orb and boss checks are **send-only** (they never receive foreign item graphics in-world): the orb simply vanishes and
a HUD line says what was sent. This avoids the hardest per-game problem in the APWorldMaker research (presenting
foreign items in the world).

## 4. Items

| Item | Count | Class | Grant |
|---|---|---|---|
| Progressive Agility / Firearms / Strength / Explosives / Driving | 4 each (= 20) | progression | set skill level and XP to that level's floor |
| Weapon unlock (per weapon) | ~15-20 | progression for the bosses that need them in logic, else useful | add to the armory |
| Supply point unlock (teleport) | ~15 | useful (option) | mark the supply point as owned |
| Skill XP bundle ("+X driving XP", the owner's idea) | filler | filler | add XP, clamp at the next level floor minus 1 so bundles never level up on their own |
| Health/ammo refill | filler | filler | refill |
| Traps: gang hit squad spawns, ammo drain, vehicle spawn under you ("car rain"), screen shake | option | trap | script/spawn call |

**[decide]** whether XP bundles can level up skills (more random, less predictable logic) or stay capped as above.
With 800 orb locations and ~60 real items, most of the pool is filler, so a good filler mix matters. Plan: weights in
the options (`filler_xp_weight`, `filler_refill_weight`, `trap_percentage`).

## 5. Logic (rules, in rule_builder terms; APWorldMaker tier T1)

- Regions: Agency island, La Mugre (Los Muertos), Volk island, Shai-Gen island; inside each, coarse height bands
  (ground, rooftop, high rooftop/tower).
- Orbs: the requirement comes from the orb's height above the nearest walkable surface and its distance from
  climbable geometry, which can't be fully computed. Phase 0 exports orb positions; a first pass uses height bands
  (ground: none; rooftops up to N m: Agility 1-2; towers: Agility 3-4; the Agency tower spire: Agility 4). The owner
  playtests and corrects the bands, which then become a static table in the world.
- Bosses: generals need Firearms 1 or Explosives 1 (combat); kingpins need Firearms 2 + Agility 2 (fortress
  access/combat), plus whatever Phase 0 shows the fortresses physically need (e.g. a high entrance).
- Races: rooftop races need Agility (by height band); road races need Driving.
- Goal **[decide]**: `goal = kingpins` (kill all 3) / `bosses` (all ~21) / `orbs N` (collect N orbs). Default kingpins.

## 6. Runtime design (inside `crackdown.exe`)

```
crackdown/src/ap/
  ap_client.cpp      connection (apclientpp, MIT, + its websocket dependency), on its own thread
  ap_state.cpp       received-items queue -> applied on the game thread at a safe point (once per frame hook,
                     sub_826A7698, which we already hook); persisted received index
  ap_checks.cpp      hooks that detect checks -> LocationChecks
  ap_grants.cpp      item -> game state writes (skill, armory, supply points, refill, traps)
  ap_hud.cpp         HUD text ("Sent Hookshot to Alice", "Received Firearms 2"), using the game's own message
                     function if Phase 0 finds it, else the ImGui overlay from roadmap step 3
```

- Settings: server, slot, password in `crackdown.toml` (cvars `ap_server`, `ap_slot`, `ap_password`), set from the
  launcher (new tab) or on the command line.
- Persistence: received index and the seed/slot are stored in a sidecar file next to the save
  (`<user_data>/ap/<seed>_<slot>.json`), keyed by the save slot. The save format isn't known, and a sidecar avoids
  corrupting it. On connect: resend all locally known checks (offline catch-up), then apply items past the stored index.
- Location IDs: stable, derived from the game's own IDs (orb index, boss id) plus a base offset, generated from the
  Phase 0 data export into both the C++ table and the Python world, by one script, so the two can't drift.
- DeathLink (option): send on agent death (hook the death/respawn function); on receive, kill the agent.
- Thread safety: the network thread never touches guest memory; everything goes through the queue drained on the game
  thread.

## 7. World (Python, in APWorldMaker)

`games/crackdown/world/` as a normal `.apworld` (no native deps): items.py, locations.py (generated), regions.py,
rules.py (rule_builder), options.py, `__init__.py`. Slot data: options the client needs (skill_mode, orb options,
DeathLink). Test with the toolkit's generation tests plus a playthrough check that every location is reachable.

## 8. Phase 0: reverse engineering (blocking; record everything in docs/re-notes.md with evidence)

Find, with guest addresses and layouts:
1. **Orbs**: the orb table (id, type agility/hidden, position), the collect function, where "collected" is stored
   (save bitfield?), and the XP award call. Method: teleport next to an orb (`--teleport`), record a short pickup
   replay, diff guest memory before/after (add a `--diag_memdiff` range dump), and trace which function writes it.
   The orb count HUD string ("Agility Orbs" / counts) is a good xref start in the generated C++.
2. **Skills**: the five XP values and levels in the player/save struct, the award function, the level thresholds
   table.
3. **Weapons/armory**: the weapon definition table and the "collected into armory" flags; supply-point weapon menu
   reading them.
4. **Bosses**: the target list (dossier data in the UI files is a good xref), the killed flag, the kill event.
5. **Supply points, races, stunts**: tables and completion flags.
6. **Scripting**: earlier notes found a script VM with bindings (re-notes: "Vehicles", RetHealth binding at
   sub_82186DF0). If events go through script bindings, one hook on the binding dispatcher may cover several check
   types.
7. **HUD message** function for our text.
8. **Save format**: only enough to tie the sidecar to a save slot.

Data-loading hooks are preferred over hardcoded patches (CLAUDE.md), so the tables should be read at load, not
patched in the XEX.

Who: Claude Code does the runtime work on the PC (teleport/replay/memdiff). Codex (read-only) and agy can do the
static side on the server: string xrefs in the generated C++, call graphs around candidates, summarizing large
functions.

## 9. Milestones

1. **M0 RE** (above): orbs and skills first; a demo where picking up one orb logs a check id and gives no XP.
2. **M1 vertical slice**: AP connection, orbs as checks, Progressive Agility as items, local test server, DeathLink.
3. **M2 full pool**: weapons, bosses, supply points, races, stunts, traps, HUD text.
4. **M3 world**: logic bands from playtests, goal options, generation tests; launcher AP tab.
5. **M4 beta** with AP players.

Codex can implement M1-M3 pieces on `wip/ap-*` branches from this plan once M0 has produced the tables; Claude Code
integrates, runs the PC tests and merges.
