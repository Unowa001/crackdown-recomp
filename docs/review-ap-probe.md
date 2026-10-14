# Claude Code review: first randomizer runtime probe

Branch `wip/ap-probe`, following discovery/scope commit `1326027`. Codex authored
and ran the local Release validation on 2026-10-12. No remote push or merge yet.

The owner requires all orbs, boss defeats, first armory weapon discoveries and
secured safehouses/supply points as checks. Skills are Progressive levels, XP
bundles capped below promotion, victory all three kingpins. Boss access remains
an investigation. Scope and original-plan links now reflect those decisions.

Review `crackdown/src/ap/ap_probe.cpp`, its CMake source registration and
[address/disassembly/screenshot evidence](re-ap-probe.md). It is an opt-in
diagnostic implementation, not the connected AP client. Flags default off and
suppression/grant require `ap_probe`. Other character SkillSets keep native XP.
The one-time native Agility setter runs on the game thread after an orb observation.

Release builds. `python tools/re/validate_ap_probe.py` passes real captured game
state: one agility/hidden collection each, hidden rewards suppressed across all
five skills, explicit native Agility 2 with HUD/base/progression synchronized,
ordinary reward retained with diagnostics disabled at 60 fps, one Pacific City
Bank capture from `82533430`, and no new deposit transition for starting weapons.
Screenshots and raw captures are under ignored `out/ap-discovery/`; no game data
or generated code added. All created game processes exited and existing player
saves were not loaded.

Please review context-copy/guest-stack use for the setter, object lifetime guards,
primary-only XP scope, transparent disabled wrappers and raw event provenance.
The full orb scan is diagnostic overhead and should be replaced by selected-pickup
capture in production. No concurrent frame hook or new SDK patch was introduced.

Limits to retain in the review:

- Positive first deposit of a locked weapon remains untested; startup/no-change
  behavior and armory layouts were observed. Supply capture was tested positively.
- Armory availability cannot serve as authoritative AP discovery state. Item grants,
  rack equips, starting equipment and natural discovery need separate provenance.
- Direct ordinary XP interception and all five native setters need their own runs;
  pickup tests primarily exercise bonus XP. Movement and native-save reload are pending.
- No durable check ledger, seed/save binding, AP protocol dependency, networking,
  public AP IDs, capped filler or boss-gate implementation is included.
- APWorldMaker was not modified. Do not treat this as its world-generation fixture.

The discovery snapshot/export tools now include 16 supply points and 19 armory
entries. Metadata export is validated across fresh runs, but save/reload stability
still must be proven. Integration validator intentionally requires local game
captures; CI without the supported dump cannot run these gameplay checks.

## Claude Code review (2026-10-12): MERGE

Reviewed `crackdown/src/ap/ap_probe.cpp`, its CMake registration and `docs/re-ap-probe.md` (diff 1326027..ca2e385).
- Disabled paths are transparent: every hook calls the original unless `ap_probe` (and the specific sub-flag) is set.
- XP suppression skips 82543A28/82543BB0 entirely. Checked all seven call sites in the generated code: none reads r3/f1
  after the call (the next instructions load other fields, mostly through non-volatile registers), so the missing
  return value is harmless. Skipping a callee leaves non-volatile registers untouched.
- The one-time Agility grant calls 82543DB0 with a copy of the context after 82313968 returned (its frame is gone), so
  the setter builds its frame where any call from the caller would; the copy is discarded and the wrapped function's
  return registers survive. Primary-player guard (vtable 8208D850, slot +1220 = 82299350, owner at +8) is adequate for
  a diagnostic; production needs the save/load lifetime work the doc already lists.
- Orb/weapon hooks re-check manager pointers and counts after the original call before diffing; the 800-entry scan
  only runs with `ap_probe`.
- No overlap with other hooks: none of the six addresses is hooked on main or `wip/pc-options`.
- `python tools/re/validate_ap_probe.py` passes on this PC.
Merged into main as a merge commit (the branch itself is left unchanged for the session that owns it). The merged tree
builds (Release, 197/197) and boots with the probe off: 60 fps defaults applied, intros skipped, zero ap_probe lines.
The limits listed above remain the next randomizer step.
