# Crackdown Archipelago scope

Draft implementation scope, 2026-10-12, on `wip/rando-scope`.
The owner authorized discovery, which is now recorded in
[discovery results](re-discovery-randomizer.md): 500 agility orbs, 300 hidden orbs,
21 bosses, measured pickup/XP transitions and a boss-specific eligibility candidate.
The installer is with Claude Code for review. This document does not edit
APWorldMaker. Runtime contract probes now live on `wip/ap-probe`; see
[probe evidence and limitations](re-ap-probe.md). They are opt-in developer
diagnostics exercised with isolated saves, not a connected randomized session.

## Owner decisions

- Skill levels come from Progressive Agility, Firearms, Strength, Explosives and
  Driving items. Gameplay XP awards are disabled in randomized saves.
- XP bundles are filler and cannot cross the next level threshold.
- Default checks include every agility orb, every hidden orb, first weapon
  discoveries into the armory, and every secured safehouse/supply point.
- Victory requires defeating all three kingpins in the bound randomized save.
- Investigate hard gates for generals/kingpins, including their spawn logic.
  The gate mechanism and item granularity are still to be selected and proven.

The original [design plan](plan-randomizer.md) remains the feature backlog.
Its counts, skill thresholds, boss combat requirements and orb logic bands are
estimates, not verified game data.

## First full playable release

Start at the Agency tower with all islands traversable. Include all orbs,
general/kingpin defeats, first weapon discoveries and secured safehouses/supply
points as checks. World interactions
send checks; they do not grant their ordinary XP or armory rewards. Boss kills
retain their vanilla effects on gang weakening and world state. Received weapon
items control armory access. Starting equipment must be explicitly granted by
slot data so the player can reach initial checks; choose it after testing the
baseline agent's combat and movement.

Use Progressive Skill items and weapon unlocks as the principal pool. Boss access
items join the pool if the gating spike passes. Health/ammo and capped XP bundles
provide filler after their grant paths are proven. The expected large orb pool
makes filler pacing a playtest requirement; do not assume ~800 checks makes a
good default session without testing it. Confirm skill-level counts before fixing
the number of Progressive items. The 20-item total in the original plan is provisional.

The owner selected rooftop races, road races and unique stunt rings as the next
check expansion on 2026-10-13: one first-success check per course/ring, with
independent group toggles. See [the expansion plan](plan-races-stunts.md) for native
evidence and acceptance criteria. These adapters are not implemented yet.

Achievement checks, traps, additional goals and co-op remain further increments.
DeathLink is optional and follows proof of the death
and respawn hooks; it does not block the first connection/item slice.

## M0: establish the game contract

| Work | Required evidence | Acceptance |
|---|---|---|
| Orb enumeration | Stable ID, type, position, load path and collected-state layout | Exact counts; IDs stable across reload and streaming order |
| Orb pickup | Guest address, call arguments, state transition and XP call chain | One pickup logs one ID; the orb disappears; its XP reward is suppressed |
| Skills | All five XP/level fields, thresholds, award path and recalculation effects | An explicit agility grant updates movement/HUD; survives save/load |
| Weapons and safehouses | Armory discovery and supply-point securing events, native IDs, load/grant paths | First discovery/capture emits one check; receiving access emits none |
| Save/session identity | Save selection/load/new-game boundaries and profile identity | AP state binds to one save; a different seed/save is rejected before grants |
| Boss gates | One general and one kingpin lifecycle, activation/spawn/defeat conditions | See the separate gating spike below |

Do the orb and skill rows first. Diagnose whether XP affects stats continuously
within a level; capped bundles may still affect gameplay, and must not silently
be classified as harmless filler if that changes access rules.

Capture a short orb-pickup replay and bounded before/after guest-memory samples.
The existing `--dump_image` dumps the executable image; it is not a live heap diff
or a sufficient substitute for player-state snapshots. Add diagnostic sampling
only around evidence-backed candidate ranges. Record disassembly, guest addresses,
field layouts and Windows/Xenia validation in `docs/re-notes.md` before merging
any hook. Preserve AP-disabled vanilla behavior and the working 30/60 fps modes.

## Boss-gating spike

The requested gate is a real encounter restriction, beyond an AP logic rule that
merely assumes the player will wait. Candidate approaches, in order to investigate:

1. Intercept the boss-specific eligibility/activation condition that the existing
   mission or spawn script consults. Return unavailable while the gate is closed;
   let the normal lifecycle proceed when the corresponding item is received.
2. Intercept boss-instance creation after identifying the boss's stable definition
   ID. Determine whether spawning can be deferred without caching a permanent
   failed spawn or leaving the encounter manager waiting for a missing actor.
3. If creation cannot safely be deferred, use an encounter activation/entry trigger
   that keeps the boss inactive until the gate opens. Its physical access behavior
   must be demonstrated; a dossier/map marker alone does not constitute a gate.

Do not implement locks by setting killed flags: that would weaken gangs, satisfy
victory checks or prevent a future legitimate spawn. Invulnerability is not the
preferred fallback because it leaves a misleading encounter available to fight.
Access items must not themselves emit boss-kill checks or alter gang weakening.

Test one general and one kingpin before extending the mechanism:

- Visit the fortress before receiving access; the boss cannot be defeated and
  no check, defeated flag or gang-weakening effect occurs.
- Receive access while already inside, then while away/outside the streamed area.
  Each case creates/activates exactly one legitimate encounter.
- Die, reload and leave/re-enter the district with both open and closed gates.
- Kill the boss after unlocking; one check is emitted and normal weakening occurs.
- Receive duplicate access items; the boss does not respawn or duplicate rewards.
- Load a save with a defeated boss; its AP check is reconciled only within the
  correct seed/save binding, and unlocking does not resurrect it.

Proposed initial pool: three individual kingpin access items. If the same lifecycle
supports generals, add optional per-general access items next. Skill-based gates
are an alternative owner choice. Final gate rules and names remain pending that
choice and the spike result. A boss gate restricts that encounter, not every orb
or weapon in its district. Fill rules must prevent a required key from being
reachable only through its own gated encounter.

If neither spawn nor activation can be safely gated, report the concrete failure
and return to the owner before substituting soft gates or removing the feature.

## M1: connected orb/skill slice

Build a small developer world with verified orb IDs and Progressive Agility.
This fixture is for proving the bridge, not a replacement for the all-orbs preset.
The AP client lives inside `crackdown.exe`; its network thread never touches guest
memory. Validate connection/slot data and queue commands for a proven safe point
on the game thread. Reuse the existing frame-hook dispatcher rather than defining
a second competing hook for `sub_826A7698`. The frame boundary alone does not prove
player objects are valid during menus, loading, death or save transitions.

Before choosing a C++ AP/WebSocket dependency, verify licensing, Windows/Linux
builds, ws/wss support and reconnect handling against the pinned AP protocol.
Receive the slot's own items through the same delivery path as remote items, so
a locally placed item is not silently lost or granted twice.

Acceptance sequence:

1. Connect to a local test server with validated game/schema/seed/team/slot data.
2. Collect an orb; one durable check is sent and vanilla XP remains unchanged.
3. Receive Progressive Agility; grant it once at a safe gameplay boundary.
4. Disconnect, collect another orb, reconnect; the missing check is reconciled.
5. Restart and reload; previous progression remains and is not granted twice.
6. Deliver repeated packets, out-of-order batches and an index-zero full inventory;
   request resync where needed and preserve the correct effective inventory.
7. Load an older game save and test crashes around delivery/persistence; recovery
   restores permanent grants without replaying already-consumed filler.
8. Reject a mismatched seed/save and leave an AP-disabled vanilla run unchanged.

## State, IDs and recovery

Keep a versioned metadata catalog as the single source for C++ and Python IDs.
Export only metadata, with evidence provenance; no game assets or generated recomp
code enter git. Define namespace/ranges after enumeration, never by transient
spawn addresses or resource-loading order. Unselected check groups retain their IDs.

Bind durable state to seed, team, slot and a locally identified save/profile.
Store the authoritative received inventory and checked-location set, not only a
last-received index. Reconcile permanent skill/weapon/access grants idempotently
after load and reconnect. A newer sidecar plus an older game save otherwise loses
previously delivered progression. Receipt of an index-zero inventory is a protocol
resync, not permission to reapply all consumable effects.

Consumables/traps need a separately tested crash/reload delivery policy. Independent
native-save and sidecar writes cannot simply be assumed atomic. Settle and document
the policy before those effects enter the public item pool. Do not claim exactly-once
consumable delivery without evidence. Persist offline checks before assuming a send
succeeded; reconcile server-known checks and local observations on reconnect.

Check observation and item-granted state must be separate: a received weapon or
supply-point unlock must not count as discovering that weapon or securing that
supply point. Achievements require seed-specific event observations, since the
SDK also stores account/profile unlocks outside the randomized save.

## M2: release world and boss progression

Expand to all verified orb IDs, all verified bosses, first weapon discoveries,
secured safehouses/supply points and
the accepted boss-access scheme. Add the remaining Progressive Skills, weapon
grants and proven filler. Emit `StatusUpdate` only after all three bound-save
kingpins are defeated, not after three keys have been received.

The Python world will live in `APWorldMaker/games/crackdown/` under that repository's
own scope/workflow when implementation begins; this task does not modify it.
Validate pool/location balance, stable IDs, completion and early reachable checks.
Rules combine physical movement/combat requirements with the actual boss gates.
Absolute orb height is only a candidate heuristic: roofs, climbable surfaces,
vehicle assists and fortress routes require playtests before becoming logic.

Test the full enabled catalog at maximum inventory and progression placement
across multiple seeds. Validate the kingpin goal with early, late and adversarial
key placement. Add the launcher AP tab and in-game sent/received status after the
bridge works; keep connection passwords out of launcher bug-report clipboard text.

## Evidence available today

- `crackdown/src/hooks_framerate.cpp`: existing `sub_826A7698` frame hook.
- `crackdown/src/hooks_input_replay.cpp` and `hooks_diag_physics.cpp`: replay,
  teleport and player/vehicle diagnostic infrastructure.
- `docs/re-notes.md`, Streaming resources: resource vector at 0x82C631F8/FC;
  role in boss/orb loading unconfirmed.
- `docs/re-notes.md`, Vehicles: script-name references including
  `SpawnAiVehicleAtPosition` (0x8207840C); boss spawning has not been traced.
- `APWorldMaker/vendor/Archipelago/docs/network protocol.md`: received-item indices,
  index-zero inventory replacement, seed identity and check reconciliation.

Orb/skill fields and boss catalog observations are now recorded in the linked
discovery results. A working boss gate is still unproven. A read-only
ASCII scan of the three local `.dff` archives did not reveal usable boss/spawn
names; it does not establish that those tables or scripts are absent. The original
plan's `RetHealth`/0x82186DF0 lead is not backed by the current RE-notes file and
must be independently verified. Boss gating is a required investigation, not a
claimed working feature.
