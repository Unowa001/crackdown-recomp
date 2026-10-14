# M2: first real multiworld run (owner + brother, 2 PCs, one AP room)

Owner decision 2026-10-13: the first run uses the full "First full playable release" scope of
[randomizer-scope.md](randomizer-scope.md): all orbs (500 agility + 300 hidden), all secured
safehouses/supply points, first weapon discoveries (weapons deposited in safehouses), all
generals and kingpins as checks, all five Progressive Skills, weapon unlock items, filler.
**Goal: defeat the three kingpins** (StatusUpdate only after the bound save records all three).
Hard boss gates are the separate spike in the scope doc; they are not required for the first
run (kingpins stay fightable; logic treats them as reachable) and can be added later without a
sidecar format break if access items are given their own IDs now (reserved, not in the pool).

Every unit below follows the evidence rule: disassembly + guest addresses, a live run on the
PC (unattended via `tools/re/ap-live.ps1`, `--teleport`, `--input_replay`), docs, Codex review.

## Units, in dependency order

| # | Unit | Proven today | Work | Acceptance |
|---|---|---|---|---|
| 1 | Skills x5 | Setter `82543DB0` (r4 skill 0..4, r5 raw level, r6=1) proven for Agility (r4=0); XP suppression covers the whole primary SkillSet | Prove r4=1..4 (Firearms, Strength, Explosives, Driving: which index is which, raw range, HUD/effect); generalize `AgilityTarget` to per-skill targets; catalog items for all 5 | Each skill: item → level up, HUD stars, survives save/load, no double grant |
| 2 | All orbs | Catalog has all 800 with ordinal/type/position; pickup path is generic; hidden orb 508 seen in probe | Contract = all 800; check `VerifyCatalog` passes on New Game and on load (streaming); hidden + vehicle pickups through the bridge | One check per orb, XP suppressed, 800/800 verify at bind |
| 3 | Filler | none | "XP bundle" capped below the next threshold via the ordinary award `82543BB0` (bypassing suppression for that one call), or a no-op item first | Filler never levels a skill; repeat/resync does not double-apply (consumable policy documented) |
| 4 | Supply points | Capture `823911A0`, legitimate caller LR `82533490` | Map native supply-point IDs → stable catalog IDs; observe capture → check (keep vanilla capture effect: spawn point) | One check per first capture; load reconciles from native state |
| 5 | Weapons | Deposit `82642760` (natural provenance incomplete) | RE armory unlock flags + grant path; natural deposit → check but no unlock; weapon items → unlock; starting loadout from slot_data | Deposit emits one check and does not unlock; item unlocks armory; save/load idempotent |
| 6 | Bosses + goal | 21 bosses enumerated, boss-specific eligibility candidate (re-discovery-randomizer.md) | Detect defeat per boss (stable ID) → check; kingpin defeats → goal; reconcile from native killed flags on load | Kill → one check, vanilla gang weakening intact; 3 kingpins → goal sent once |
| 7 | APWorld | M1 dev world in APWorldMaker (uncommitted) | Catalog v2 (single source C++/Python), items (5x4 skills, weapons, filler, reserved boss keys), locations, simple logic (all reachable; weapons/skills for fortress bosses as playtest dictates), slot_data, options (include hidden orbs, etc.) | Generates 2-player seeds; spoiler sane; IDs stable |
| 8 | In-game notifications | none (log only) | ImGui overlay (`OnCreateDialogs`): connection state, "Received X from Y", "Sent X to Y", halts/offline | Visible during play at 60 fps; no input capture |
| 9 | Tester build | v0.2.0 published; launcher AP tab done (wip/ap-bridge) | Bump to 0.2.1+, package, release; hosting: self-host + port forward, or upload the generated seed to archipelago.gg (verify custom-world hosting works) | Brother installs, connects, items flow both ways |

Order: 1 → 2 → 7 (world can be built in parallel once 1 fixes the item list) → 8 → 4 → 5 → 6 → 3 → 9.
A playable two-PC test can happen after 1, 2, 7, 8 (orbs + skills, goal temporarily "max skills"
behind an option) while 4-6 continue; the real run needs all of them.

Candidates for Codex implementation on its own `wip/*` branch (owner preference): unit 7 (Python
world, from this table and the catalog), unit 8 (overlay) once the bridge exposes a status queue.
Claude Code keeps RE/hooks (1-6) and live PC tests.

## Open review items carried over

Selected check expansion (2026-10-13): rooftop races, road races and unique stunt
rings, each with first-success checks and an independent toggle. Discovery and
acceptance are recorded in [plan-races-stunts.md](plan-races-stunts.md); no completed
adapter or revised location count is claimed by this addition.

From `docs/re-ap-bridge.md` round 2: save-lineage content evidence (orb-flag fingerprint) and the
pre-write gate in `sub_8254E1A0`. Neither blocks play tests; both block the merge to main.

## Game adapter implementation, 2026-10-13

The M2 contract and native supply/deposit/boss/goal adapters are implemented on
`wip/m2-adapters`; see [adapter evidence](re-m2-adapters.md) for commits, exact
native serialization timing, live logs and remaining acceptance limits. All 19
deposit identities and goal save/load/server rollback were exercised with isolated
profiles. Boss acceptance used explicit native callback diagnostics; ordinary
combat and a full 855-pickup playthrough remain unproven. Overlay, publishing and
multiplayer are outside this adapter pass. Existing save-lineage/pre-write risks
remain open.
