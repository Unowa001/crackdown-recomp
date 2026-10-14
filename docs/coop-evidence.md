# Co-op transport evidence (2026-10-13)

This is an experimental System Link implementation. Native transport tests are
not a passing two-player gameplay test.

The isolated game worktree is `out/coop` (`wip/coop`), based on `wip/plans`.
The isolated SDK is `out/sdk-coop`, pinned initially to
`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`, with inherited patches 0001–0010.
No shared SDK source files were edited. SDK commits are detached local objects
used solely to produce format-patch files; the game branch is the deliverable.

The seven planned transport patches are numbered 0011–0017 because 0010 was
already occupied. In particular 0017 is **socket readiness**, not the plan's
original session-object patch. Conditional session/XGI/XMsg/QoS work must start
at the next free number.

## Native checks

Build logs, outside git:

- `out/coop/out/sdk-build.log`: isolated Release SDK build and install.
- `out/coop/out/game-build.log`: clean codegen (0 unresolved calls) and game link.
- `out/coop/out/fixture-build.log`: `tools/coop/transport_fixture.cpp` build.
- `out/coop/out/fresh-stack.log`: apply all 17 patches to a fresh pinned SDK
  worktree (`out/sdk-coop-verify`), then independently reverse-check every patch.
- `out/coop/out/patch-roundtrip.log`: reverse/reapply each patch independently
  with the complete stack present; tracked contents remain unchanged.
- `out/coop/out/apply-stack.log`: successful `tools/apply-sdk-patches.sh` run.
- `out/coop/out/fixture/{A,B}/transport-v3.log` and `errors-v3.log`: two separate
  SDK runtimes; each finishes with `PASS key unregister cleanup`, no errors.

These exercise actual XSocket and kernel export entry functions on XHostThread,
not an alternate transport implementation: typed key generation/registration,
identical/conflicting duplicate keys, XNADDR conversion, by-value reverse token
conversion, guest sockaddr bytes, VDP mapped to UDP, broadcast options, ioctl
scalar conversion, nonblocking empty receive (10035), socket query sizes and
values, short queries and unknown handles, readiness association and timeout,
140-byte payload preservation, broadcast to explicit-peer unicast, reverse
source translation, TCP connect/accept and bidirectional bytes, unknown opaque
tokens and key cleanup.

Fixture v2 had a harness race: its timeout assertion used the same read event
on which the other process was already sending. v3 uses a separate unsignaled
event. v1 tested UDP/TCP only. Keep those logs as iteration evidence; v3 is the
expanded passing run.

Patch 0015 uses zero-context hunks around adjacent original export stubs. Both
patch appliers enable `--unidiff-zero`; other patches retain ordinary context.
No later patch edits implementation or declarations added by an earlier patch.
Corrections were folded into the owning SDK commit before regenerating patches.

## Guest and fork sources

The source checkout is `out/ref/xenia-netplay` at
`ef09406de5e16badb4ad9d3a9df452020015946a`. ABI definitions adapted from its
`src/xenia/kernel/xnet.h:472–587` and `xsession.h:73–215` retain the Xenia Canary
copyright notice. Keys use its type classification (`xnet.h:1516`) with fresh
random bytes, rather than its fixed exchange-key bytes. Address conversion
uses the by-value dword ABI in `xam/xam_net.cc:925`; reverse lookup uses local
peer records, without the fork's `FindPlayer` HTTP call.

Guest disassembly comes from local `crackdown/generated/default` (never in git)
for the title SHA256 in CLAUDE.md. `docs/plan-coop.md` section 3 records the
higher-level instructions and exact generated-file references:

| Behavior | Guest instructions |
|---|---|
| Session create, local join, remote join | 0x8292AAE0, 0x8292AC58, 0x8292AD00 |
| Create wrapper | 0x8292A9B8; info pointer in r8, handle output in r10 |
| Join wrappers | 0x8292ABF8 (local), 0x8292ACA0 (remote) |
| Start/end wrappers | 0x8292AF70, 0x8292B008 |
| Key create/register, joining register | 0x824621B4, 0x824621C4, 0x82462718 |
| Forward/reverse addresses | 0x82462758, 0x8243BA58 |
| Connect/status | 0x8241C69C, 0x824628BC |
| VDP socket/bind 1000 | 0x82462350, 0x8246238C |
| UDP socket/bind 1001 | 0x82462AF8, 0x82462B6C |
| TCP bind 1002 | 0x82462560 |
| Discovery send 140/16 bytes | 0x82462CBC, 0x82462A8C |

`out/coop/out/trace-sources.txt` records every traced network wrapper's
generated file and line. The wrapper transformations (`mr` arguments, `li
r3,1`, tail branch to import) were read before adding hooks. The game hook calls
`__imp__sub_*` exactly once; it never substitutes multiplayer state transitions.

## Current live evidence

`out/coop-live/{A,B}` have separate executables, config/log directories, user
data copies, cache copies, metadata and distinct XUID/name/MAC settings.
Read-only game assets are shared. Host offsets are 20000/21000, producing
21000–21002 and 22000–22002; explicit peers point to the opposite discovery port.
`arguments.txt` records the actual commands, and `*.rec` records menu input.

At initial boot, A's `trace.jsonl` records successful XNetStartup and WSAStartup.
`A/window-01.png` shows the title screen; `A/window-02.png` shows the main menu
after a START pulse at replay time 20000 ms. A START at 4000 ms was too early.
Window screenshots use PrintWindow, avoiding foreground/focus dependence.

System Link session creation/join, gameplay traffic and remote world changes
have **not yet been proved**. No decision to omit the conditional session/XGI/
XMsg/QoS patches can be inferred from boot-only traces.
