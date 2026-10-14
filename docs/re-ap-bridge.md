# In-process Archipelago bridge (M1) — evidence

2026-10-13, branch `wip/ap-bridge` (child of `wip/launcher-randomizer-integration`),
Claude Code. Codex review pending. XEX SHA256 `7aaa7a89…7e5a`.

## What it is

`crackdown/src/ap/`: `client.cpp` (WinHTTP WebSocket ws/wss, network thread only
queues generation-tagged packets), `bridge.cpp` (one coordinator on the existing
frame hook `sub_826A7698`; owns `SessionState`), `catalog.cpp` (embedded
`crackdown/ap/catalog.json`, also the Python world's ID source),
`session_state.cpp` (sidecar `<user_data>/archipelago/session.toml`, format 2,
adds native-save lineage). Enabled by `--ap_server=host:port --ap_slot=NAME`
(password: `<user_data>/archipelago/connection.toml`, written by the launcher, or `--ap_password`), as the launcher's
ARCHIPELAGO tab does per profile, or automatically when the profile already has a sidecar
(offline play keeps XP suppressed and checks pending). No sidecar and no server:
vanilla, no hooks act.

Linux: transport stub (reports "Windows-only"); everything else compiles.
The AP server warns that the client lacks websocket compression (WinHTTP has none).

## Guest contracts (disassembly via `tools/re/ap_discovery.py`, live trace `--ap_trace_save`)

| Address | Evidence | Bridge use |
|---|---|---|
| `824EEEE8` | pickup manager ctor; trace: once at boot, once per New Game/load | marks a world transition: binding re-decided |
| `824EF158` | orb load serializer (vtable `820B3B34`); called from native load LR `8254DB50`; never on New Game (traces `trace1`, `load1`) | hash `Crackdown.sav`, accept only if in lineage |
| `824EF1E0` | orb save serializer; stream+76≠0 on write pass, LR `8254E61C` from supply-point save `8268F0A8` | durable `save_pending`, then hash file when write settles |
| `824EF6F8` / `824EF588` | pickup queries, only caller `82313968` (call sites `82313D34`/`82313D24`); r5=r28=player; non-null r3 → `stb r25(=0),288(r27+r28)` at `82313D38` | exact selected pickup → ordinal → type/position verified → `Observe` |
| `82543A28` / `82543BB0` | XP award (bonus / ordinary) | dropped for the primary SkillSet in a bound save |
| `82543DB0` | setter r3 SkillSet r4 skill r5 raw level r6=1 | absolute Agility target, verified after call |

Save format: one `Crackdown.sav` per profile, `CRAK` chunk stream (20-byte stub
until the first supply-point save). Campaign → Play Now loads it if present.
HUD stars = raw level − 1 (1280×720 `m1-load1-086s.png`: raw 3 = two stars);
four Progressive Agility receipts reach raw 5 = four stars.

## Live runs (local AP 0.6.8 server, 2-slot seed `31315763575795839193`, logs/screens in `out/ap-live/`)

| Run | Result |
|---|---|
| `m1-orb1` | connect/validate, New Game binds save `bf5c…`, orb 178 → one check, own item → Agility 1→2 |
| `m1-save1` | restart: sidecar restores Agility 2 with no new item; supply-point save recorded in lineage |
| `m1-load1` | native load hash in lineage → bound; remote item (Partner orb 177, `/send_location`) appended → Agility 3, HUD two stars |
| `m1-offline1` | server down; older save (Agility 2) restored to 3; orb 175 offline with injected write failure → retried, persisted, pending; server back → sent, own item → 4; HUD XP 00 after pickup |
| `m1-crash1` → `m1-recover1` | observation forced to fail, native save includes the pickup, process killed; next load recovers orb 177 from native collected flags → sent → item |
| `m1-rollback1` | server restarted from older .apsave forgot a check; resent on Connected → item restored → Agility 5 (cap) |
| `m1-vanilla1` | no AP config, fresh profile: no `ap:` lines, no sidecar, orb 178 gives XP 10 |

Host tests: 6 CTest cases (adds `lineage`).

## Review round 1 (Codex `~/reviews/2026-10-13/codex_review_ap_bridge.md`, FIX FIRST)

| Finding | Change | Evidence |
|---|---|---|
| P1 pending write admits any foreign save | `BeginSave` stores `save_pending_at`; an unknown hash loads as bound only if the file was written within 300 s after it (a copied save keeps its old mtime). Old sidecars without the time never admit one | `ap_state_lineage` |
| P1 native save proceeds after `BeginSave` fails | The hook can't cancel the native save, so the save is watched anyway and `RecordSave` retries every 15 frames until the sidecar is writable (no halt) | `regress-save2` (`--ap_dev_fault=fail_begin_save`) → `regress-load2` bound |
| P1 foreign-seed halt keeps XP suppressed | Kept on purpose: a profile bound to seed A stays seed A's save whatever server it meets. Server-side rejections (seed, slot, password, contract, bad packet) now `Disconnect` → offline; only local failures `Halt` | `foreign-seed1`: seed 7244… rejected, save still bound offline, Agility restored from the sidecar |
| P1 WinHTTP handle closed during a blocked call | Receive thread owns open/receive/close; sender only sends and `WinHttpWebSocketShutdown`s (send side, may overlap a receive, per *Concurrency in WinHTTP*); close happens under the send mutex | `net-silent`, `net-stall`, all runs exit 0 |
| P1 password exposure | Launcher keeps it in `<profile>/archipelago/connection.toml`; the game reads it from there (`Password()`), so it never passes through argv, the environment or a cvar (the SDK's `cvar.cpp` debug lines 127/667 print cvar values). `--ap_password` remains for manual use. "Copy system info" redacts any `*password*` key | `pwfile-wrong` (InvalidPassword → offline) / `pwfile-right` (connected); 0 occurrences in logs |
| P2 stalled handshake blocks shutdown | Handshake has 10 s timeouts; `Stop()` waits ≤ 3 s, then detaches (threads share only a ref-counted State). An open socket's receive has no timeout (WinHTTP ignores it after upgrade; measured), WinHTTP's 30 s keepalive pings detect dead links | `net-stall`: error 12002 every ~12 s with backoff, close mid-handshake exits in 3.4 s; `net-silent`: close with an unanswered close frame exits in 3.4 s |
| P2 concurrent `Stop()` | `lifecycle_` mutex; halts call non-blocking `RequestStop()` so the game thread never joins | — |
| P2 checks sent before binding | Kept on purpose: pending checks are only ever observations from the bound save of this seed (`Observe`/`RecoverNativeChecks` require a bound save), so they are valid to send at any time | — |
| nice: acks from `Connected` | `Acknowledge(..., authoritative=true)` replaces the set; forgotten checks become ordinary pending checks | `ap_state_acknowledge` |
| nice: stable uuid | `<user_data>/archipelago/client_uuid` | — |

Regression (`regress-orb1`): fresh profile, password via env, New Game bind, orb 178 → one check → own item → Agility 1→2.
Test helpers: `tools/re/ap_fake_server.py` (stall/silent endpoints), `ap-live.ps1 -CloseWindow`.

### Round 2 (`codex_review_ap_bridge_r2.md`, FIX FIRST)

Accepted: 2a (bound save keeps rules offline), 2b (pending checks sendable), WinHTTP ownership, Stop
serialization, authoritative acks. Fixed after it: system-info redaction parses TOML; launcher always
passes empty `ap_*` overrides; in-memory uuid. **Open (residual risk, not blocking play tests):**
- An unknown save after a crash is admitted on a 300 s mtime window, not on content evidence. Planned:
  fingerprint the 800 orb flags at the save serializer (`824EF1E0`) and require the same flags after the
  load serializer (`824EF158`).
- If `BeginSave` fails and the game crashes before the retried `RecordSave` succeeds (retry every 15
  frames), the save loads as foreign. A gate before the native write needs RE of `sub_8254E1A0`
  (contains LR `8254E61C`; body in `crackdown_recomp.71.cpp`) and its failure handling.
- A peer that ignores the close frame keeps a detached receive thread alive until exit.

## Not yet exercised live

Foreign native save (load hash not in lineage → vanilla rules) is implemented
but not run live; duplicate
/ gap packets via `--ap_dev_fault=duplicate_items|gap_items` not run live (unit
tested). Jump-height effect of Agility not measured. Hidden-orb and vehicle
pickup paths not run through the bridge. In-game overlay/notifications: none yet
(log only).
