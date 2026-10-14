# Crackdown RE notes

Game-specific findings: guest addresses, data layouts, and what they're for. The general method is in `playbook.md`.
Orb/skill/boss discovery from isolated Windows runs on 2026-10-12 is recorded in
[randomizer discovery results](re-discovery-randomizer.md), with static/runtime
evidence and the remaining hook-validation steps.
Every address here is for `Default.xex` sha256 `7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a`.
Mark each entry **confirmed** (checked against a Xenia trace or a screenshot) or **unconfirmed** (from disassembly or a dump only).

## Kernel-object helpers

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x82A4FA60 | `SetEvent(ev)`: `li r5,0; li r4,0; b __imp__KeSetEvent` | disassembly | unconfirmed (disassembly) |
| 0x82A4FA70 | `ResetEvent(ev)`, inlined: `li r11,0; stw r11,4(r3); blr`. Never calls the kernel. Also a folded C++ method in vtable slots at 0x8203503C / 0x8203505C (slot 0) and 0x8207C698 (slot 3 of 0x8207C68C) (non-KEVENT `this`). **Hooked**, guarded (`crackdown/src/hooks_events.cpp`) | disassembly; dump 2026-10-09; PC New Game run 2026-10-10 (log crackdown_009) | confirmed (hook passes) |
| 0x82A4FA80 | `WaitForMultipleObjectsEx(count, objs, waitAll, ms, alertable)` → `__imp__KeWaitForMultipleObjects`. `ms = -1` means infinite, otherwise ms × -10000 (a relative timeout in 100 ns units) | disassembly | unconfirmed (disassembly) |
| 0x827461A8 | Wait wrapper → `NtWaitForSingleObjectEx` (the guest main thread blocks here while loading) | stack dump | unconfirmed |

## HDD cache (utility drive)

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x82322F28 | Game init. If byte `0x82BA7B2A` is set, calls `XMountUtilityDrive(1, 0x10000, 0x10000)`; on 0 it sets byte **`0x82DE3F9A` = cache available** (read at 9 sites) | disassembly | unconfirmed (disassembly) |
| 0x827457E8 | `XMountUtilityDrive` wrapper (statically linked XDK): `sub_82744E70` (reads a 32-bit ID, no side effects), then `sub_82745458`. **Hooked**: returns `ERROR_DEVICE_NOT_CONNECTED` (`crackdown/src/hooks_storage.cpp`) | disassembly; trace log 2026-10-09; PC New Game run 2026-10-10 (log crackdown_009) | confirmed (hook passes) |
| 0x82745458 | The mount itself: opens the raw cache partition, reads 1 KiB at offset 2 KiB and checks `0x4A6F7368` ("Josh"), returns a Win32 error from `RtlNtStatusToDosError` | disassembly | unconfirmed (disassembly) |
| 0x82745110 | Cache worker thread. All `StfsCreateDevice` (2) / `StfsControlDevice` (5) calls; links `cache:` → `\Device\cache0` | disassembly; trace | unconfirmed (disassembly) |

- With the cache on, the game copies `Streaming\fullcity_0100_EnvironmentBlocks.resblockc` to `cache:\fullcity_0100_EnvironmentBlocksc` at boot, and on New Game opens `cache:\globalc`, `cache:\fullcityc` and `cache:\fullcity_0100_EnvironmentBlocksc`. The trailing `c` means the compressed archive.
- Randomizer note: with the cache off, every level archive is read straight from `game:\` (`D:\*.dff`, `D:\Streaming\*.resblock[c]`), so substituting data can be done at the file level.

## Streaming resources (named load tasks)

The `"FinalDefault"` scheduler (object at `0x830B6F58`, functions `sub_8253xxxx`) holds about 1,857 resource objects. The pointer vector is at `0x82C631F8` (begin) / `0x82C631FC` (end). Each object has a UTF-16 name at +0x04, for example `"Ren_Male Character 2"` or `"Phy_d04_b008"`, and a type name at +0x11C (`"Phy_cStaticEnvironment"`). **Likely the key table for randomizing spawns and pickups. Look for name prefixes beyond Ren_/Phy_.**

| Offset | Meaning (from the dump; partly guessed) |
|---|---|
| +0x16C | state: 0 init, 1 waiting (main), 2 assigned, 3, 4 running, 5 done, 6 cancel?, 7 idle/yielded, 8 |
| +0x178 | uncompressed size (e.g. 0xA6800) |
| +0x17C | compressed read length (e.g. 0x6B800) |
| +0x190 | offset in the `.resblockc` (e.g. 0x0724D800) |
| +0x10C | other offset (0x1A41F000, fits the uncompressed `.resblock`) |

- `sub_825398F0`: synchronous load. Re-posts the task while state is 1/7, waiting on the scheduler event at +60 (`F8000048`).
- `sub_8253BBA0` / `sub_8253B9E8`: workers. They run `sub_825399E8(task)` and set state 5 when it reports done, otherwise 7.

## Job dispatcher (possibly the streaming loader): sub_8225C538

A thread that serves 8 request slots. **Unconfirmed** whether these are streaming / `.resblock` reads. Check with a Xenia trace before relying on it. If they are, this is a candidate place to intercept data *loading* for the randomizer.

- Main loop at 0x8225C580. With an empty queue it waits forever on all 17 objects. With work queued it polls (timeout 0), and only on `WAIT_TIMEOUT` (258) does it pop the ring and call `sub_8225BF00(slot)` to start the job.
- Wait index 16 means shutdown (it closes each slot's handle at +12 via `sub_82A499C0`). Index `2k`/`2k+1` is slot k's event pair. An odd index resets both events and sets `state = 2`.
- Globals (base 0x82D30000):
  - `0x82D33BC0`: ring tail (u32). `0x82D33BC4`: ring head (u32).
  - `0x82D33BC8`: ring of 9 slot pointers (index mod 9, through `mulhwu 0x38E38E39`).
  - `0x82D33BEC + 4k`: per-slot u32 (cleared through the slot's +64 pointer).
  - `0x82D33C10`: 17 KEVENT pointers (0–15: notification events in pairs; 16: synchronization/quit event).
  - `0x82D33C58`: 8 slots × 88 bytes.
  - `0x82D336F4`: CRITICAL_SECTION guarding the ring (r24 = `lis -32045` + 14068).
- Slot layout (88 bytes, from the dump; field meanings guessed):

| Offset | Seen | Notes |
|---|---|---|
| +0x00 | 0x0044AA20 | ? |
| +0x04 | 0x405C6C10 | pointer |
| +0x08 | 0x1B | ? |
| +0x0C | 0x4073EED4… | handle/object, closed on shutdown |
| +0x10 | 3 / 0 / 2 | ? |
| +0x18 | 1 | state (set to 2 on completion) |
| +0x1C | 0xFF5A4800 + 0x28000·k | per-slot buffer, 160 KB each (physical range) |
| +0x38 / +0x3C | | copied +0x38 → +0x3C on completion |
| +0x40 | 0x82D33BEC + 4k | pointer to the per-slot u32 |
| +0x44 / +0x48 | &events[2k], &events[2k+1] | the slot's event pair |
| +0x4C..+0x51 | bytes | flags, cleared on completion |

- Hang snapshot (before the hook): `tail=1, head=0`, `ring[0]=0x82D33C58` (slot 0 queued, never started), all 17 guest `SignalState = 0`, and the dispatcher thread at about 80% CPU.

## Audio (XAudio mix, 3D sound)

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x82A50000 | XAudio multi-core mix barrier `(engine r3, word r4)`. Each worker writes 1 into byte `KPCR+0x10C` (cpu number, `lbz r11,268(r13)`) of the 8-byte word, then spins at loc_82A500DC until the word equals `expected` (byte i set for each i in 0..5 with `[engine+308+4*i] != 0`; byte 0 = MSB) or 0. The worker that sees it complete stores 0. Skipped when `[engine+304] == 0`. **Hooked** (`crackdown/src/hooks_xaudio.cpp`) | disassembly; live cdb 2026-10-11: workers on cpu 4 and 5 spinning, expected `0x0000000001010000`, word `0x0000000001000000` | hook in testing |
| 0x82A509D8 | Per-audio-frame mix step that calls the barrier | disassembly | unconfirmed (disassembly) |
| 0x82A50FD8 / 0x82A510A0 | XAudio mix worker thread bodies | cdb stacks 2026-10-11 | unconfirmed |
| 0x82A51150 | Audio callback; waits on the workers (`KeWaitForMultipleObjects`, no timeout) | cdb stacks 2026-10-11 | unconfirmed |
| 0x82252150 | 3D sound code that feeds a normalized dot product into the arccos below (exact role unconfirmed) | NaN store/birth probes 2026-10-11 | unconfirmed |
| 0x8225F910 | arccos approximation; returns NaN for an input just above 1.0 (this is where the `vrsqrtefp` bug became NaN audio) | NaN probes; 9.5 min session clean after SDK patch 0002 | confirmed (fix verified in game) |

## Rendering

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x826012D0, 0x82719CA0 | Hold most of the ~130 in-place `vpkd3d128 vX,vX,5,2,2` (float16_4) packs; the NPC head mesh is packed to float16 here every frame | RenderDoc `f11_1.rdc` (head draw ev 5720 vs body 5717); heads render after SDK patch 0001 (user check 2026-10-11) | confirmed |
| 0x826BCA98 | Present routine, the only caller of `VdSwap`. **Hooked** for the F11 RenderDoc capture (`crackdown/src/hooks_capture.cpp`, only active when launched from RenderDoc) | disassembly (import xrefs) | unconfirmed (disassembly) |
| 0x82BAA330 | s32 frame limiter cap, used by sub_826A65F0 / sub_826A7698 | disassembly | unconfirmed (disassembly) |
| 0x820ED704 | float 1/30, the simulation step (main loop sub_826A7698 scales it by [0x82CFE4B4]). **Written** each frame with the measured frame time, clamped 1/240..1/20 s, by `--sim_variable_step` (`crackdown/src/hooks_framerate.cpp`, `host_tuning.cpp`) | disassembly; PC run 2026-10-11 21:04 (`crackdown_035`, `varstep.csv`): 191 s at `--fps_cap=60`, 10 s averages 42 to 62 fps, 6 frames over the 50 ms clamp; user: game speed steady through the dips, "only minimal slowdown" (orb pickups included) | confirmed (game speed); physics not yet checked |
| 0x82DE25C0 | Pointer to the game clock object (constructor sub_823225C0, vtable 0x82095D5C). +13 = "locked" byte, always set to 1 by the constructor: while set, eleven inlined timer sites (sub_824F9518, sub_824FAC28, sub_8257A4F8, sub_82388D90, sub_82505CB8, sub_82287130, sub_8248F210, sub_825E11D0, sub_823F0E70, sub_8259A330, sub_82651570) and the end of sub_826A7698 add a fixed 33 ms per frame, the step query sub_8254C568 uses 0x820ED704, and the frame limiter runs. +14 = paused(?); +16 = frame counter, bumped by sub_82324938 only while locked, read by ~13 sites (mostly the 0x8246xxxx cluster, small thresholds and +60 deadlines). The 33 literals are **hooked** (`config/midasm_hooks.toml`, `--sim_game_clock`: measured frame ms, clock stays locked). Clearing +13 (with variable mode and a matching step) broke physics: player thrown into a wall, cars flying (PC run `crackdown_018`, 2026-10-11; no other write to the clock seen during frames) | disassembly 2026-10-11; symptoms at 60 fps with only the step constant changed (`crackdown_039`): speech lines cut off, Agency car elevator fast | unconfirmed (disassembly) |
| 0x82DE3FB0 / 0x82D99228 | u8 "variable ms" mode flag / u32 ms for this frame. When the clock is unlocked: flag set gives ms = [0x82D99228] (step query: ms * 0.001), clear gives 16 ms (step query: 1/60 from 0x820ED634). Written by the clock's vtable methods sub_82326348 / sub_823263D8. Not used: unlocking the clock breaks physics (row above) | disassembly 2026-10-11 | unconfirmed (disassembly) |
| 0x1D953180.. (physical, per query) | Occlusion query sample-count structs for light coronas: END's struct is 0x20 bytes before BEGIN's, consecutive queries 0x80 apart | F10 occlusion log 2026-10-11 (crackdown_020) | confirmed (fix verified in game) |

## Player character (controller, Havok character proxy)

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x822A9E78 | Character controller update `(controller r3, dt f1)`. Proxy at `[controller+660]`; controller position at +336 (x,y,z; +340 = height), velocity built at +352, vertical speed accumulator at +532 (`+= [+372] * dt`, capped by `[+392]`). Early branch to 0x822AB5CC (player pinned, e.g. spawn fade-in) only does `setPosition(proxy, controller+336)`. Else: `vel = move`, `+= out` of sub_822B4100 (step up) or sub_822B4D40 (step down), `vel.y += [+532]`, then sub_822ACF68. **Hooked** for `--teleport` (`crackdown/src/hooks_diag_physics.cpp`) | disassembly; guest back-chain from the integrate hook (`crackdown_049`): sub_822ACF68 <- sub_822A9E78 <- sub_822B97B0 <- sub_822A0C30 <- sub_82299C00 <- sub_823928F8 <- sub_82710498 <- main update sub_826A6B50; teleport via +336 works (`crackdown_053`) | confirmed |
| 0x822ACF68 | `(controller r3, dt f1)`: hkStepInfo `{0, 0, dt, 1/dt}`, `setLinearVelocity(proxy, controller+352)` (sub_829D3580), then `proxy->integrate` | disassembly; `--diag_char_proxy` logs dt/inv consistent at 30 and 60 fps (`crackdown_046`, `_048`) | confirmed |
| 0x822B4D40 | Step-down / stick-to-ground `(controller r3, dt f1, out r5) -> bool`. Tail at 0x822B562C: `out.y = -clamp(target - height, 0, max) * gain / dt`. A stair step is a fixed height, so the pull doubles at 60 fps; the proxy solver turns part of it into forward speed at stair edges (**60 fps stairs speed bug**). **Hooked**: `--sim_ground_snap_30hz` (default on) scales `out.y` by `dt * 30` when dt < 1/30 (`crackdown/src/hooks_framerate.cpp`) | disassembly; replayed stairs run from a teleport (`bench/stairs_tp.rec`, `--teleport=1396.6,19.8,-1413.1`), peak horizontal speed: 30 fps 11.86 m/s (`crackdown_053`), 60 fps fix off 24.41 (`_054`), fix on 11.96 (`_055`); also the full replayed walk (`bench/stairs.rec`): 60 fps peaks 31.9 (`_049`) -> 11.3 (`_050`) | confirmed (replay A/B); user play test pending |
| 0x822B4100 | Step-up counterpart, writes a height to `out.y` (no dt divide) | disassembly | unconfirmed (disassembly) |
| 0x822BF268 | `hkCharacterProxy::integrateWithCollectors` (Havok utilities linked into the game section; .pdata 0x820F66C8, vtable slot 0x8208A780): r3 = proxy, r4 = hkStepInfo `{start, end, deltaTime, invDeltaTime}`, r5 = unused-zero vector here. Loops up to 10 casts on deltaTime; at 0x822BFDC0 stores the solver velocity to proxy+16 and calls the phantom's setPosition. Proxy +48 = phantom; position = `[[proxy+48]+28]+48` (sub_829D3550); setPosition = sub_829D3560. Only one proxy is integrated in the city (the player). **Hooked** for `--diag_char_proxy` and the replay gameplay gate | disassembly ("updateCharacter" timer); `crackdown_046` | confirmed |
| 0x822BE368 | `hkCharacterProxy::integrate(stepInfo, gravity)`: builds two point collectors and calls the vtable slot +12 (integrateWithCollectors) | disassembly | unconfirmed (disassembly) |

## Vehicles (Havok vehicle kit, game classes)

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x8204FF58 | `hkVehicleInstance` vtable (Havok vehicle kit; an hkUnaryAction). Stored by the finish-loading constructor sub_829ED808, which the type-info table at 0x82BA2568 (`name, finish, 0` rows) pairs with the name string 0x8205000C | disassembly; table rows | unconfirmed (disassembly) |
| 0x829ED828 | Base vtable slot +8: `hkVehicleInstance::applyAction(stepInfo r4)`, loads `deltaTime = [r4+8]` at entry. Never runs for the game's cars (overridden, below) | disassembly; 0 calls in 40 s of replayed city gameplay (`crackdown_056`, `_057`) | confirmed |
| 0x824A0670 / 0x820B5100 / 0x824A1708 | The game's vehicle class: constructor sub_824A0670 calls the hkVehicleInstance constructor sub_829ECB30, then stores vtable 0x820B5100, whose slot +8 (applyAction) is **sub_824A1708**. Steps every physical car (traffic, parked, Agency garage, the player's). Its components are embedded in a shared per-model profile object built by sub_824AB418 (data at +496, driverInput +1156, steering +48, engine +124, transmission +276, brake +0, suspension +224, aerodynamics +400, velocityDamper +1136). **Hooked** for `--diag_vehicle` / `--teleport_vehicle` (`crackdown/src/hooks_diag_physics.cpp`) | disassembly; `--diag_vehicle` logs plausible positions/speeds (traffic 10-18 m/s, player car accelerating under RT, `crackdown_058`-`_072`) | confirmed |
| — | Chassis axes: local +z is forward (car driven with RT moves along rotation column 2), +y up | `--diag_vehicle` xaxis/zaxis, `crackdown_070` | confirmed |
| — | Member offsets from Havok's reflection member tables (`tools/diag/hkclass.py`; the hkClass objects are built at static init, the member arrays are plain data): hkVehicleInstance +24 entity, +28 data, +32 driverInput, +68 chassis, +76 wheelsInfo, +160 deviceStatus, +184 torque, +188 rpm, +192 mainSteeringAngle, +209 currentGear; hkEntity +80 motion (pointer); hkMotion +16 motionState (translation at +64), +208 linearVelocity, +224 angularVelocity; hkWorldCinfo +92 expectedMinPsiDeltaTime | member tables 0x820512B8 (vehicle), 0x8204B610 (entity), 0x8204D448 (motion), 0x82BA21A0 (world cinfo) | confirmed (reflection data) |
| 0x824C2008 / 0x824C54D8 / 0x824CF798 | Class-info singletons for `cCar` (object size 7072, class id 16), `cLandVehicle`, `cVehicle`; `cVehicleController` 0x82597548 (size 2304), `CCarController` 0x82593708, `cVehicleDamageProp` 0x8251DDB0. Each builds its info once: `sub_8237D588(id, name, size)` | disassembly (`tools/diag/strref.py` on the class-name strings) | unconfirmed (disassembly) |

- 60 fps check (2026-10-12, unattended replays, `bench/car_tp.rec`: teleport next to an Agency garage car at spawn, Y to enter, full RT): with `--sim_game_clock --sim_step_literal` the car accelerates the same at 30 and 60 fps (7.9/14.1/18.5 m/s at 0.5/1.0/1.5 s at 30 fps vs 7.3/13.1/18.1 at 60, `crackdown_061`/`_062`) and takes the same wall hit (18 vs 17 m/s velocity change). Without `--sim_step_literal` it is ahead at 1 s (15.3 m/s, `_069`): the "a little fast" driving. Damage, read off the HUD vehicle health bar 1.5-4 s after the hit: none to a few % at either rate in the garage (`_067`/`_068`/`_069`); down the Agency stairs (`--teleport_vehicle=1396.6,19.8,-1420,0`) ~12% at 30 fps after a 26 m/s crash and ~12% at 60 fps after hitting the fence at the bottom at 15 m/s (`_071`/`_072`). No instant wreck reproduced in any configuration.
- Randomizer note: vehicle spawning goes through script functions (`SpawnAiVehicleAtPosition` 0x8207840C, `SummonAIVehicleAtPos` 0x820783F4, `SpawnNPCIntoVehicle` 0x82075FF4, `Config\NPC\GenericVehicleSpawnScript` 0x8207887C); not traced yet.

## Controller input

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x82743798 | `XInputGetState(user r3, state r4)`: tail-calls XamInputGetState (import 0x82B67074). **Hooked** for `--input_record` / `--input_replay` (`crackdown/src/hooks_input_replay.cpp`) | disassembly; replay verified (`crackdown_045`, `_047`) | confirmed |
| 0x827437C0 | `XInputGetKeystroke(user r3, flags r4, keystroke r5)`: user 255 -> XINPUT_FLAG_ANY_USER, then XamInputGetKeystrokeEx (0x82B67094). Menus ("Press START") read this, not the pad state. **Hooked** as above | disassembly; START via replayed keystroke leaves the title screen (`crackdown_045`) | confirmed |

## Boot and file I/O

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x82742B28 | Statically linked `CreateFileA`: r3 = path, r4 = access, r5 = share, r7 = disposition (1..5), r8 = flags. A bad disposition calls sub_82746010 (`NTSTATUS` → `SetLastError`) with 0xC000000D and returns -1. **Hooked**: fails `MSGS.bik` / `RTW_Logo.bik` (boot logos, opened as `d:\XUIFrontEnd\…`) with `STATUS_OBJECT_NAME_NOT_FOUND`, cvar `--skip_intro_videos` (`crackdown/src/hooks_intro.cpp`); logs every `.bik` open | disassembly; runtime log 2026-10-10 19:48 and 2026-10-11 01:39 (`intro: open d:\xuifrontend\msgs.bik -> skipped`, then `rtw_logo.bik`; boot continues with `crackdownhd.bik`) | confirmed |
| 0x82746010 | `SetLastError(RtlNtStatusToDosError(r3))` | disassembly | unconfirmed (disassembly) |

- Randomizer note: every file open goes through 0x82742B28, so it is also the place to redirect whole files (e.g. a substituted `.resblock`) without touching the loaders.

## Randomizer runtime contract probe

See [RE probe evidence](re-ap-probe.md) for orb collection/XP wrappers, the native
Agility setter trial, supply-point capture at `0x823911A0` (caller `0x82533490`),
and the armory deposit candidate `0x82642760`. Fresh-game layouts enumerate 16
supply points and 19 armory entries. Positive first deposit of a locked weapon,
save/reload recovery and boss access gates remain unproven. No AP checks are sent yet.

## XUI pause/menu PC actions (milestone 1, 2026-10-12)

Static analysis from Claude's `claude_xui_code.md`, checked against the local
reference packages and generated disassembly. Runtime: PC test 2026-10-12 (Release, log crackdown_002): menu redirect, both presses, mode-2 popup, No/Yes and clean exit confirmed;
see the receipt in [pc-options.md](pc-options.md). See [pc-options.md](pc-options.md) for regeneration and tests.

| Address | What | Evidence | Status |
|---|---|---|---|
| 0x8262A4F0 / 0x8262A198 | Screen registry / loader: 77 is menuHud_3.xur, 76 is extractPopUp | static registry/loader analysis; local XZP entries | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x8237A240 / 0x82621990 | With HDD-cache flag 0x82DE3F9A clear, standalone `game:\XUIFrontEnd\menuHUD_3.xzp` is read whole, then mounted as memory://; the Scene.xzp copy is for the cache path | generated calls to 0x82742B28 (CreateFileA), 0x82743DF0 (GetFileSizeEx), 0x82742D18 (ReadFile); format `memory://%X,%X` at 0x820DD93C; storage hook keeps flag clear | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x82742B28 | Sole CreateFileA hook also redirects the standalone menu to read-only pcmod: | generated .52.cpp: `mr r29,r3; mr r4,r29; addi r3,r1,104; bl 0x82b673c4` (RtlInitAnsiString), then OBJECT_ATTRIBUTES and `bl 0x82b673b4` at 0x82742C78 (NtCreateFile); failure sets LastError via 0x82746010 and returns -1 | redirect confirmed on PC 2026-10-12 (crackdown_002) |
| 0x8269C3C0 | Shared ObjectProc, r3=hObj, r4=message, r5=wrapper; hooked for the new Ids | generated .47.cpp: `lwz r11,0(r5); mr r3,r5; lwz r11,0(r11); mtctr r11; bctr`; wrapper registration 0x8261C828 | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x826E1CA8 | XuiElementGetId(hObj, UTF-16 Id out-pointer) | generated .49.cpp: `mr r8,r4; stw r11,0(r8)` initializes output; success `lwz r11,4(r3); li r3,0; stw r11,0(r8)` | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x8260A628 | Button-wrapper XM_PRESS=2021; handled flag +8; returns HRESULT 0 | generated .43.cpp: `lwz r10,4(r31); cmplwi cr6,r10,2021`, callback at wrapper+52, then `li r11,1; stw r11,8(r31); li r3,0` | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x82678768 | Existing Tab2 Quit opens popup mode 0; quitGame reproduces its setup with mode 2 | generated .46.cpp: `bl 0x8268d318; li r11,0; stw r11,4(r3); bl 0x823508f8; li r7,0; li r6,0; li r5,0; li r4,76; bl 0x82628918; li r3,1`. Parent r7=0, unlike Tab1 callbacks | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x8268D318 / 0x835D3BB0 | Popup-mode singleton, mode u32 at +4 | getter generated .46.cpp; reads in 0x826587F8 and popup initialization | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x823508F8 / 0x82628918 | Menu manager / ShowScreen(manager,id,0,0,parent) | argument setup in generated 0x82678768 above | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x82658278 / 0x82692990 | Popup constructor / title selection: mode 0 title 655, mode >=2 title 656 (Quit); mode-2 footer ids 0 | static popup analysis and GameStrings.str index | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x826587F8 | Popup Yes: mode 0 save/Main Menu, 1 Extract, 2 host shutdown when mod active | generated .45.cpp: `bl 0x8268d318; lwz r11,4(r3); cmplwi cr6,r11,1; blt cr6,0x82658874; bne cr6,0x82658880`; mode 0 `bl 0x82691308; li r4,1; bl 0x82672dd8`; all end with `li r3,1`. Mode >=2 originally has no action | confirmed on PC 2026-10-12 (crackdown_002) |
| 0x826586B0 | Popup No goes Back; retained original | static popup callback analysis | confirmed on PC 2026-10-12 (crackdown_002) |

menuHUD_3.xzp is 64,591 bytes (SHA256
`b505b2db97dee625527715a5a3a36a677ffd9a639109248fa4a999b7953f6fce`).
Its menu scene's Tab1 has `controllerSettings -> liveSetup -> audioSetup ->
displaySetup -> pcSettings -> gameCredits`, Tab2 has `campaign -> timeTrials ->
DLC -> extract -> quit -> quitGame`, both cyclic with reverse NavUp. The new
controls have literal Text and empty PressPath, with copied opacity timelines.
The original quit retains its PressPath and callback. Screen localization comes
from `Local/<lang>/XUIFrontEnd/Scene.xzp#menuHud_3.xus`, keyed by element path;
new Ids should keep literal Text (runtime check pending). Milestone 1 applied only the standalone patch. Milestone 2 below adds the native
Scene package; localization patches remain prospective recipe outputs.


## XUI native PC Settings (milestone 2, 2026-10-13)

### Property mask and SDK revision failures (Windows, 2026-10-12)

Pass-through diagnostic hooks in owner runs `crackdown_007.log` and
`crackdown_008.log` show ShowScreen 0x82628918 resolving `options.xur` to
`file://game:/xuifrontend/scene.xzp#options.xur`. Resolution succeeds, but
XuiSceneCreate 0x826E0BE8 returns `0x8030000D` with a null scene. The first
failure is recursive element parser 0x826FDCC8, propagated through 0x826FC560.

Disassembly of property reader 0x826FCC50 at 0x826FCCD8..0x826FCD1C checks
mask widths 1, 2 and 4. Width 3 takes `bne cr6,0x826fd07c`, skipping all
properties rather than consuming their values. 0x826FDCC8 subsequently compares
the parsed property count with the declared count at 0x826FDEA0..0x826FDEB8
and returns `0x8030000D` on mismatch. The new captions/dummy wrappers use
Hittable/DisableFocusRecursion, producing 3-byte masks under the old editor.

The width-only fix still failed in `crackdown_009.log`. Element-level tracing
in `crackdown_010.log` identifies the first new caption (after both framerate
radio buttons): it declares 12 properties, but the Element class reader
consumes only 5. Its Hittable/DisableFocusRecursion bits are newer, previously
unverified schema candidates that this game's class does not consume. Their
unconsumed values also desynchronize the following Control/Label descriptors.
The recipe removes both fields and uses supported Enabled/UnfocusedInput
properties to keep captions and hidden ctor dummies out of input navigation.

After removing those fields, `crackdown_011.log` reads 11 of 12 caption
properties. Startup registration tracing through 0x826D8F18 in
`crackdown_012.log` establishes the real field tables: Element has 14 fields,
Control has 11 (Text=8, PointSize=9, ImagePath=10), Scene has 7, Button has 1,
and Label has none. The previous schema inserted two nonexistent navigation
fields ahead of Text, so the recipe wrote its PointSize outside the table.
Schema/recipes/independent wire fixtures now use the verified ordinals and
reject unsupported fields before serialization.

The editor rounds 3-byte masks to 4 bytes; strict XUR parsing rejects
unsupported mask widths. No guest functions are replaced by this fix. The
regenerated Scene patch is 3276 bytes, 1300 literal bytes, with zero original
literal windows of 16 bytes or longer; all original package entries are copied
unchanged. Result SHA256:
`a539651e377bd1a96c176079b75d0cd7ba10ec42ddd5f91a9265ba2ad3265f95`.
The standalone menu is regenerated too: Text now uses the actual field 8,
and bogus ImagePath values formerly added as "Text" are no longer introduced.
Its result is `9fb7a17599c80e7dd3daf60b7165045c2ecf17438e4f09fcc3448ed382060c01`.
All 30 synthetic XUI tests
pass, including unsupported-width rejection and 4-byte serialization.
Owner run `crackdown_013.log` creates the scene with HRESULT 0, initializes
screen 26, saves resolution_scale=2 from a code-4 resolution notification,
and reopens with resolution selection 1. Owner reports that it worked; the
2026-10-12 22:45:14 screenshot proves the native page renders. The screenshot
also exposes decorative title graphics cloned into captions, so the recipe
now uses the existing plain XuiLabel style for the heading/captions instead.
Visual-cleanup retest and explicit music confirmation remain pending.

Static design: `~/reviews/2026-10-12-xui/claude_screen.md` (Claude static analysis,
2026-10-12), independently checked against generated PPC disassembly by Codex.
**All new behavior is unverified at runtime.** The 2026-10-12 Windows receipt
covers only milestone 1. See [pc-options.md](pc-options.md) for the Windows test,
settings/save policy and patch audit. No new Xenia trace or screenshot exists;
record the PC build/log/screenshot receipt before merge, per CLAUDE.md.

| Address | What | Disassembly evidence | Status |
|---|---|---|---|
| 0x8262A4F0 / 0x8268FA40 | Existing registry entry 26: name `options`, file `options.xur`, original factory | registry analysis in claude_screen.md; factory .47.cpp allocates 144 bytes and calls 0x8266B928 | unverified at runtime |
| 0x8266B928 | Hook calls original options ctor, then initializes native radio selections | .45.cpp:76162: `mr r31,r3; mr r30,r5; bl 0x82627528`; AgentProfiles `bl 0x82690f68` at 0x8266B9C4, then `lwz r3,4(r3); bl 0x826fa9e0`; AudioSetup lookup at 0x8266B9FC, `addi r3,r30,52; bl 0x82696898` subscribes 0x82678BB0 | unverified at runtime |
| 0x82690F68 | Child-wrapper lookup does not safely handle absent children | ctor above immediately dereferences the returned wrapper; original AgentProfiles label and AudioSetup button IDs at 0x820E4D70 / 0x820E4D8C (claude_screen.md); both hidden/disabled wrappers retained in options.xur | unverified at runtime |
| 0x826275A0 | Base screen object stores id +128, scene wrapper +132 | .43.cpp:70496: `stw r11,128(r31); stw r5,132(r31)` | unverified at runtime for id 26 |
| 0x82678C88 / 0x8268DC40 / 0x823508F8 / 0x82628918 | New press reproduces Tab1 Display's ShowScreen setup, replaces id 74 with 26 | .46.cpp:26622: `bl 0x8268dc40; lwz r31,4(r3); bl 0x823508f8; mr r7,r31; li r6,0; li r5,0; li r4,74; bl 0x82628918` | unverified at runtime |
| 0x826E1D00 | XuiElementGetChildById(parent hObj, UTF-16 name, output hObj) | .49.cpp:52116: `mr r28,r4; mr r30,r5; stw r29,0(r30); lwz r31,24(r3); bl 0x82ab9d98; stw r3,0(r30)` | unverified at runtime |
| 0x8269B5D8 / 0x8269B620 | Set current radio selection by hObj/index, XM 2008 | .47.cpp:30069/30112: `mr r31,r3; mr r5,r4; bl 0x8269b620; mr r3,r31; bl 0x826d9790`; builder `li r4,2008; stw r30,0(r31)` | unverified at runtime |
| 0x82628C78 | Manager stack uses 12-byte items; last object's pointer is end-4 | .43.cpp:73983: `addi r3,r28,36; bl 0x8267d0d0; lwz r11,44(r28); lwz r31,-4(r11)`; hook guards empty stack (begin +40, end +44) before reading id +128 | unverified at runtime |
| 0x8269C3C0 / 0x8260A068 | Shared ObjectProc now logs every XM_NOTIFY on screen 26; code 4 reads/saves a changed known group | .47.cpp:32205 original wrapper dispatch; .43.cpp:641: `cmplwi cr6,r10,14; lwz r9,16(r31); lwz r9,0(r9); cmplwi cr6,r9,4; addi r3,r11,52`; code 2 instead dispatches +92 | unverified at runtime; actual PC codes must be logged |
| 0x8269C378 / 0x826FA928 | Get current radio selection, XM 2007 | .47.cpp:32162: `bl 0x826fa928; mr r3,r31; bl 0x826d9790; lwz r3,80(r1)`; .50.cpp:34676 builder `li r4,2007` | unverified at runtime |
| 0x82624120 / 0x82624198 | Focus guard / focused-child index; latter loops forever with no focus | .43.cpp:62698/62772: guard stops when 0x826FA6E0 returns null; index loop 0x826241CC only stops when 0x826E0418 reports focus. Neither helper is used by the new page; any future index read must guard first | static hazard confirmed; avoided |
| 0x826ED538 / 0x826E1318 / 0x82629008 | Native scene Back and destruction pop screen stack; no new Back hook | .50.cpp:2778: notify/press checks XuiBackButton and calls 0x826E1318 at 0x826ED858; original scene msg 10 unregisters via screen manager | unverified at runtime for id 26 |
| 0x8261F740 / 0x8261FC80 | vfs resolver indexes package entries dynamically; new options.xur needs no registry patch | .43.cpp:52042/52789: package open `bl 0x826e7520`, enumerate `bl 0x82700168`, close `bl 0x826ffd48`; localized miss falls back to global Scene | unverified at runtime |
| 0x82700948 / 0x82742B28 | Global Scene opens through the existing CreateFileA hook | .50.cpp:49007: 0x82700A60 `lhz r9,0(r11); stb r9,0(r10)` converts UTF-16 file path to ANSI; 0x82700A94 `bl 0x82742b28`; 0x826FF708 (.50.cpp:46262) reads package header/index through this stream | new Scene redirect unverified at runtime |

Scene.xzp receives only `options.xur` (root Id `options`, ClassOverride `scene`).
Every existing global entry is unchanged, including menuHud_3.xur. Its patch is
3,334 bytes with zero original-matching literal windows >=16 bytes. Original
SHA256 `9ca02bfedd706fce30197156edeb7aeea6751f0cdd6c111fac56ef020ae5f95d`, result
`d79282ce2fc99c181ab785388027eb73a02d98624f97afb59933d5c5cd6bc20b`.
The 25,914,713-byte input and result are hash-checked each startup; cumulative
SHA256/preparation milliseconds and cache reuse/rebuild are logged. Only the exact
global package path redirects; localized Scene.xzp is untouched.

New-page state is pending until restart, seeded from effective startup cvars;
only changed keys use the launcher's shared toml++ source-preserving editor. The
SDK's fullscreen callback (`sdk/src/ui/rex_app.cpp`, pinned f5337cd:353) directly
calls Window::SetFullscreen, so it is not invoked from these guest hooks. Defaults
remove vsync/resolution_scale/fullscreen keys. Advanced fps/simulation/per-axis
scale settings and CLI overrides remain intact and can override preset semantics.
Literal English options labels have no existing options.xus; future locale paths
are listed in pc-options.md. Footer hints are hidden; XuiBackButton has visible
Back text for mouse use, retaining the native controller PressKey 22593.
