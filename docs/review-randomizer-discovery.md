# Review handoff: randomizer discovery

Branch: `wip/rando-scope`. Author: Codex. Claude Code review pending.
Owner approved discovery after selecting all orbs, Progressive Skill levels with
capped XP filler and the three-kingpin goal, and requesting boss-gating research.

Read [discovery results](re-discovery-randomizer.md) and
[implementation scope](randomizer-scope.md). Review only documentation and
`tools/re/` in this discovery change. No runtime hook or SDK patch was added.
Raw image, heap samples, logs and isolated diagnostic saves remain local under
ignored `out/ap-discovery/`; the metadata export is `catalog.json` there.

Measured results:

- 800 ordered pickups: 500 agility (four variants), 300 hidden.
- Array order, type and position match across independent fresh games; save/load
  ordinal stability is still an implementation gate.
- Agility ordinal 178 and hidden ordinal 508 each produced exactly one availability
  transition and the matching counter increment. Both vanilla XP paths were observed.
- All five skill banks/indices, baseline levels and RPG threshold tables located.
- 21 boss IDs/names repeat across fresh games. General 0 and kingpin candidate 3
  each acquired a live actor after visiting their recorded position, with dead flag 0.
- Both share the boss-specific state/eligibility path at `sub_823E9248`; no locked
  or deferred-unlock behavior has been tested yet.

Check disassembly against the recorded addresses, especially the XP two-bank
semantics, native level setter's side effects and the relationship between the
eligibility predicate, actor spawning and retry behavior. An inferred script
binding or a successful memory read is not sufficient evidence for a safe hook.

Next runtime work should be separate reviewed commits: one check/no-XP pickup,
one native Progressive Agility grant, and the closed/open general/kingpin eligibility
experiment. Keep native kill/gang-weakening effects, verify AP-disabled behavior,
and prove save binding/recovery before the AP network slice. Do not merge the
launcher or publish a beta based on this handoff alone.
