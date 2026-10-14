# M1 state core and client implementation handoff

2026-10-12. `crackdown/src/ap/session_state.{h,cpp}` implements the host-side
permanent-item/check state for the connected orb/Agility developer slice. It is
compiled into the game source list, but has no runtime consumer yet. No connected
session, public IDs, consumable delivery or new game hooks are claimed.

## Implemented contract

- A version-1 TOML sidecar binds game/schema/seed/team/slot/profile/save/catalog
  before any event is accepted. The Agility item ID, selected location set and
  starting/maximum raw levels are also persisted and checked on reopen. Corrupt,
  incompatible and mismatched files are rejected without overwriting them.
- Every accepted item batch, local observation and server acknowledgment commits
  through the existing exclusive-temp, flush, atomic-replacement writer before
  changing memory. Persistence failure throws and preserves the previous state.
  Keep a single coordinator/process owning a bound save; this class does not
  implement a cross-process lock. File replacement is not a transaction with the
  native save, and this test does not prove hardware power-loss durability.
- Index zero replaces the whole received inventory, including an empty/shorter
  snapshot. A nonzero batch is accepted only at the next exact inventory index.
  Duplicates, overlaps and gaps request resync without partially applying items;
  grant planning pauses until an index-zero snapshot commits. No deduplication by
  item ID, sender or location: two identical receipts at different valid indices
  are two items. Own-slot and remote items use the same path.
- The M1 catalog admits only Progressive Agility. Unknown items, unsupported flags,
  invalid indices and excessive packet/state sizes throw. The adapter must stop
  the session on these validation errors; do not catch them and continue granting.
- Observed checks and server acknowledgments are separate durable sets.
  `Observe` commits before the check can be sent. `PendingChecks` returns observed
  minus acknowledged; a socket send never removes a pending check. Server checks
  from a partner can be acknowledged before a local observation. Initial native
  flags and received items do not automatically produce observations.
- `AgilityTarget(active_binding, native_level, gameplay_ready)` returns an absolute
  native raw level, capped at five, only for a known inventory and a ready bound
  save. Repeat it after native loads and item changes. It returns no work if the
  current level matches; a full inventory replacement can also lower the target.
  Starting Agility defaults to raw level one; four receipts reach raw level five.
  This is an M1 runtime contract, not approval of a final world pool or star count.

The item resync behavior follows the upstream
[AP network protocol](https://github.com/ArchipelagoMW/Archipelago/blob/main/docs/network%20protocol.md):
send `Sync`, then `LocationChecks`, after an index mismatch, and accept an index-zero
inventory as replacement. The transport remains to be pinned/implemented.

## Adapter sequence for Claude

1. Prove native save/profile identity and save/load lifecycle first. A filename,
   menu slot number, player address or current process ID is insufficient. Generate
   and persist a save UUID at a demonstrated new-save boundary; carry it across
   legitimate native save updates and older snapshots. Reject a different native
   save before XP suppression, item application or observation. Do not invent an
   identity from an unverified field. Save UUID embedding/association is unresolved.
2. Pin the AP/WebSocket implementation and build Windows/Linux ws/wss/compression.
   Validate RoomInfo seed, Connected team/slot/game, and versioned slot_data against
   the M1 catalog before constructing `Contract`. Request all own and remote items
   (`items_handling = 7`) and slot data. The sidecar contains no server password.
3. Keep sockets off guest memory. Queue immutable packets tagged with a connection
   generation. One coordinator owns `SessionState`; drop callbacks from old socket
   generations after disconnect/reconnect. Clear gameplay readiness before native
   load/death/menu transitions and pause on any validation/persistence exception.
4. Construct/reopen the sidecar for the validated bound save. Supply the catalog's
   stable public IDs; the tests' 900001/910001-style IDs are fictional fixtures.
   No APWorldMaker repository has been edited. Build the small developer world
   under that repository's own workflow using independently verified orb IDs.
5. Feed full/contiguous `ReceivedItems` to `Receive`. On `resync_required`, send
   `Sync` followed by pending `LocationChecks`, await index-zero inventory, and
   retry offline checks until Connected/RoomUpdate confirms them. Feed only the
   selected catalog's validated server check IDs into `Acknowledge`. On restart
   after a resync, authenticate and request a fresh full inventory before enabling
   gameplay; the transient resync flag is intentionally not persisted.
6. Use the existing frame dispatcher at `sub_826A7698` for queued permanent grant
   work after proving player/save lifetime safety. The diagnostic PrimarySkills
   vtable/owner guard alone does not prove this. Read the active native Agility,
   obtain `AgilityTarget`, then call proven setter `82543DB0` with the copied PPC
   context and r6=1. Setter failures/missing lifetime evidence leave reconciliation
   pending; no sidecar "applied" cursor marks an unverified native effect complete.
   Reconcile again after every old-save load. Same-level targets skip the setter,
   which clears XP banks. Disable ordinary gameplay XP only in the active bound
   randomized save, with the AP-disabled path preserving vanilla behavior.
7. Replace the diagnostic 800-orb snapshot scan with a proven selected-pickup event
   and stable-ID mapping. Only a natural primary-player pickup in the bound save
   calls `Observe`. Persist before dispatch. If observation persistence fails after
   the native pickup, pause and retain/retry the event in memory; prove recovery
   from the bound native collected flags after a crash before claiming no lost
   checks. This core does not solve that native-event/persistence window.
8. Run M1's live acceptance sequence in `randomizer-scope.md`: own and remote
   items, offline pickups, reconnect, repeated/out-of-order packets, restart,
   older saves, crashes around delivery, and foreign-seed/save refusal. Then add
   the launcher AP tab. Boss gates, weapons, other skills and filler remain later
   game-contract work; they are not silently accepted by this M1 state core.

## Verification

Standalone build avoids configuring the recompilation/SDK runtime:

```powershell
cmake -S crackdown/src/ap -B out/ap-state -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDK_THIRDPARTY=sdk/thirdparty
cmake --build out/ap-state
ctest --test-dir out/ap-state --output-on-failure
```

Use the VS x64 compiler environment; locally Clang 23.1.3 built it. Five Windows
CTest cases pass: binding, receipts, recovery, checks, persistence. They exercise
all binding dimensions, contract drift, own/remote items, initial gaps, duplicate
and replay handling, empty replacement, cap enforcement, unsupported input,
restart/older-save target calculation, readiness refusal, offline/partner check
acknowledgment, failed write rollback, retry, corrupt-state refusal and temporary
cleanup. Tests simulate native levels; live movement/HUD/save behavior still
requires the game adapter and owner acceptance. Existing toml++ deprecation
warnings are from the SDK dependency.
