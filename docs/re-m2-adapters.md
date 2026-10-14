# M2 game adapters (2026-10-13, Codex)

Contract commit embeds `tools/re/make_ap_catalog.py --m2` output exactly equal to
`out/apworld-m2/games/crackdown/world/crackdown/data/catalog.json` (read only).
Full/hidden-off slots require exactly 855/555 locations, five skills, 19 weapons,
no-op filler 73411000 and the declared goal/three kingpin locations. Receipts and
starting availability never create deposit observations. Sidecar contract fields
are optional extensions to format 2, retaining index-zero replacement, resync
and native-save binding behavior. 10/10 tests: `out/ap-live/m2-contract-ctest.log`;
Release: `out/ap-live/m2-contract-build.log`.

Supply hook `823911A0` retains the entire native body and return/register effects.
Only primary player 0, index 0..14, changed return, LR `82533490` creates a check.
Supported table guard: `82DE2678`, count +8 = 16, position +64+64*index,
primary captured flag +2616+index. Bound loads reconcile flags; Agency index 15
has no check. RE: `crackdown_recomp.149.cpp` (`823911A0`) and
`crackdown_recomp.57.cpp` (`82533430`, call at `8253348C`). Extracted disassembly:
`out/ap-live/m2-supply-disassembly.txt`. Live `m2-supply-network2.log`:
855-location M2 connection; index 0/player 0/LR 82533490, one check 73401000,
server acknowledgement and filler receipt index 4; native save recorded as 10453
bytes, hash prefix 83e8a987620ad293. Screens `m2-supply-network2-075s.png` and
`-090s.png` under `out/ap-live/`. Host provenance tests: 11/11 in
`m2-supply-ctest.log`; Release `m2-supply-build.log`.

All live profiles are isolated copies under this worktree's `out/ap-live/`.
The local M2 server uses copied seed/world/host files in `out/ap-live/m2-server/`
and the authorized Python 3.13 venv; the world source/SDK are not modified.
`m2-supply-network1.log` is an unsuccessful invocation (PowerShell array passed
as a single native argument); it is not acceptance evidence.

Supply recovery: `out/ap-live/m2-supply-recovery2.log` loads native hash
83e8a987620ad293 as bound, verifies all 800 orbs, and restores missing observation
73401000 from the captured flag. Only the copied recovery sidecar was edited
(`observed = []`); no new capture or grant was injected. `-075s.png` is the load
screenshot. `m2-supply-recovery1.log` used an insufficient menu replay and did not
reach gameplay; it is not acceptance evidence.

Weapons: `82642760` calls `822D8FD0` then `82644A50` for every carried firearm,
and `82644A50` for character+6308 throwable. Single direct caller `82642950`,
call `8264299C`, LR `826429A0`. Resolver LRs `826427FC`/`8264284C` are observed
only within a thread-local natural primary deposit pass. This captures starting
and AP-unlocked weapons with no flag transition. The native body runs unchanged;
all armory flags are temporarily 1 only during that synchronous scan so native
code skips unlocks/popup additions; RAII restores the prior flags even on C++
exceptions, then reconciles +9 solely from inventory/starting flags. Receipt
resync suspends reconciliation; deposits remain checks. Supported armory guard:
`835D3F84`, +12/+16 vector exactly 19 x112, IDs/index and +8 starting flags.
Guest sources `crackdown_recomp.104.cpp`, `.118.cpp`, `.169.cpp`, `.34.cpp`;
`out/ap-live/m2-deposit-disassembly.txt`, `m2-deposit-callers.jsonl`.

Live `m2-weapons-network1.log`: bound load, supply 1 captured, native carried
IDs 1/2/18 each observed/acknowledged as one deposit check despite already being
available. All 15 nonstarting items were delivered via local AP `/send` console
commands. The first run showed grants deferred in paused supply UI, fixed by
reconciling armory flags in a bound world with valid primary layout (skill setter
still requires advancing updates). `m2-weapons-offline1.log`: older native save
f2e6981918847cd6 loads bound, all 15 nonstarting entries restored 0->1 from durable
receipts without any deposit check. Snapshot directories
`m2-weapons-before-receipts/` (four starting flags only) and
`m2-weapons-offline-restored/` (all 19 available, no sampling errors).
Screens: `m2-weapons-network1-075s.png`, `-105s.png`,
`m2-weapons-offline1-075s.png`. 11/11 CTest `m2-weapons-ctest.log`;
Release `m2-weapons-build3.log`. `m2-weapons-build.log` failed to relink while
an earlier live test held the exe; builds 2/3 succeeded after that test exited.

Not yet live-proven: all 19 individual deposits, a naturally acquired nonstarting
weapon remaining locked after deposit, receipt/armory selection while its UI is
open, or recovery of an unpersisted deposit across a crash. Armory flags cannot
recover deposit provenance; sidecar observations/retries are authoritative.

Boss/goal adapters: `823CBA98` always calls the complete original native body
before observing boss +32 native ID and +148 dead flag. Table guard `82DE27F4`,
end +192 exactly manager+208+21*4, 21 unique catalog IDs, vtable `820A513C`,
bool dead bytes. Bound loads recover each native dead boss's check. Native
`cBossInfo` save interface is at boss+28, vtable `820A515C`; serializer
`823CC1A8` writes offsets +116..120 (boss +144..148). The new `8254E1A0` wrapper
retains the entire native save body and collects the actual primary write-pass
boss payload. The orb serializer is insufficient goal evidence: in live traces,
a save *inside* `823CBA98` serializes the prior dead flag before the callback
sets +148. Only a complete 21-boss payload with all kingpins 3/11/17 dead can
be attached to the settled native file hash. A bound load can reconstruct this
witness from the loaded native flags if its sidecar goal field was lost.

`SessionState::RecordSave` atomically persists lineage and optional goal witness.
Observed checks, server acknowledgements and inventory do not complete victory.
Witness is monotonic across older bound loads; receipt resync does not erase it.
`StatusUpdate` status 30 (`CLIENT_GOAL`) sends once per validated connection,
and replays on reconnect/server rollback (protocol has no delivery ack).
Stored offline contracts now also validate items, skills, goal and location groups
against the embedded catalog; no automatic binding migration is attempted.

RE before save-pipeline changes: `out/ap-live/m2-save-pipeline-disassembly.txt`
(`crackdown_recomp.71.cpp`, `8254E1A0`, original serializer loop/stream disposal/
write job). Boss callback `.3.cpp`, load serializer `.126.cpp`, save `.20.cpp`:
`out/ap-live/m2-boss-disassembly.txt`. Existing native pre-write failure and
300-second mtime crash-lineage risks are unchanged; no save cancellation gate is
claimed. Host goal tests exercise incomplete native defeats, items/checks/acks
not satisfying victory, write failures, durable offline restore, older native
loads, foreign bindings and the optional max-skills goal. 12/12 CTest:
`out/ap-live/m2-goal-ctest2.log`. Release `m2-serialized-goal-build.log`.

Live `m2-serialized-goal1.log` (isolated copy, local full M2 seed): real AP receipts
raise all five skills 1->5; opt-in diagnostic callbacks for native IDs 3/11/17
execute the full original bodies and native saves. Serializer traces go
false/false/false -> true/false/false -> true/true/false -> true/true/true.
Only the final serialized payload/hash 44a4be31e86d521a produces CLIENT_GOAL;
server `out/ap-live/m2-server/server.log` records StatusUpdate 30 and completion.
Screens `-080s.png`, `-140s.png`, `-210s.png`. `m2-boss-goal1.log` additionally
proves general 0's native body/check/save. These are callback diagnostics,
**not combat kills**. No boss gate or balanced combat/reachability proof is claimed.

`m2-goal-recovery1.log` (no probe enabled): copied sidecar had all boss
observations removed and `goal_save = ''`; native hash 44a4be31e86d521a loads
bound, checks 73402003/11/17 and the goal witness recover from native flags.
This independently proves that the completed native save contains all three dead
kingpins. Screenshot `m2-goal-recovery1-075s.png`.

Diagnostic helpers require `--ap_probe` plus an explicit weapon/boss ID; defaults
have no effect. Weapon helper uses `823925C0`'s bounded definition registry,
`82354108` display name/class guards and native `822B7210` pickup. RE
`out/ap-live/m2-addweapon-disassembly.txt`. `m2-weapon4-withheld3.log` / `-080s.png`:
native-carried ID 4 is deposited through LR 826429A0; check 73403004 persists,
+9 remains 0 with only the five original receipts. Snapshot
`m2-weapon4-after-deposit/snapshot.json` independently confirms it. Native pickup
was diagnostic, but deposit invocation/provenance was the ordinary supply UI.
`m2-allweapons-deposit1.log` proves carried deposits for every firearm ID
1/2/4..16 plus throwable 18, including starting weapons without transitions.
No AP receipts were injected in that offline profile.

Discarded diagnostic attempts are retained as failure evidence, not acceptance:
`m2-boss3-probe1` and `m2-boss-goal1` callback snapshots exposed the pre-dead save
ordering; `m2-native-goal1/2` attempted an unproven native damage dispatch, the
latter hit a guest read at 0x44. That damage helper was removed. Weapon probe
attempts `m2-weapon4-withheld1/2` failed definition lookup due to signed-address
calculation and do not prove nonstarting deposits; attempt 3 is the valid run.

Rollback/reconnect proof: `out/ap-live/m2-goal-server-rollback1.log` loads the
same completed native save with probes off against a separate local server at
38292 restored from `m2-server/pre-goal.apsave.bak`. Forgotten boss checks resend
and CLIENT_GOAL sends once after binding. `m2-rollback-server/server.log` records
the packet and completion; `-075s.png` captures the run. The test seed/server's
automatic goal collection then checks the remaining locations and delivers an
874-entry full inventory (pool + precollected items + prior console grants).
All M2 item IDs/filler receipts are accepted and skills cap at 5. Automatic server
collection is not evidence of native orb pickups or local deposit observations.
Final host run: 12/12 `out/ap-live/m2-final-ctest.log`.

Final throwable coverage: native `82644B38` (safehouse loadout selection,
player r3, armory entry r4, category r5=2, r6=1) owns the active throwable at
character+6308; native addweapon alone does not change that selection. Opt-in
probe now uses that native selector for throwable diagnostics. Disassembly
`out/ap-live/m2-throwable-selection-disassembly.txt` (`crackdown_recomp.96.cpp`).
`m2-throwable3-deposit1.log`, `m2-throwable17-deposit1.log`,
`m2-throwable19-deposit1.log` / each `-080s.png` show subsequent ordinary deposits
at caller LR 826429A0 and durable checks 73403003/17/19. All three use separate
copies of the all-weapons test profile; starting ID 3 has its precollected receipt,
17/19 have none. Union with `m2-allweapons-deposit1.log` covers every native ID
1..19. Repeated firearms 1/2 in these loads do not create new observations.
These prove scanner/identity/provenance coverage, with diagnostic acquisition/
selection; they do not prove every enemy-drop or UI-only acquisition route.
Release `out/ap-live/m2-throwable-build.log` succeeds; all three live runs exit 0.
