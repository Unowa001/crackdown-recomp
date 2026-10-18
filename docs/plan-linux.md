# Playable Linux build implementation plan

Target document: `docs/plan-linux.md`  
Research date: 2026-10-13  
Scope: native Linux x86-64, Vulkan, playable single-player game, launcher, and distributable packages.

This plan is based on static source inspection. No files were modified and no builds or runtime tests were performed. The integration checkout is at `639f203`; the inspected SDK identifies as v0.10.0. All patches 0001–0009 independently passed `git apply --reverse --check` against that SDK.

**The Linux port already has substantial SDK support. The principal renderer work is Vulkan parity for patch 0005 and patch 0008, followed by runtime validation. Patch 0007 already benefits Vulkan.** Linking successfully does not establish that guest memory, signals, rendering, audio, or gameplay work.

Evidence paths below use:

- `sdk/…`: the SDK submodule in the main checkout.
- `crackdown/…`, `patches/…`, `tools/…`: relative to the `out/full-integration/` worktree.
- `CLAUDE.md`: the workspace-root document read first.

## 1. Linux support in the SDK today

| Area | Implemented support and limitations | Evidence |
|---|---|---|
| GPU backend | Non-Windows builds disable D3D12 and default Vulkan on. The xenos shared plugin includes a real Vulkan command processor, texture cache, render-target cache, and SPIR-V pipeline implementation. | `sdk/CMakeLists.txt:38`, `sdk/src/graphics/CMakeLists.txt:44`, `sdk/src/graphics/CMakeLists.txt:88` |
| Backend selection | The application loads `gpu_plugin=xenos`; its default backend argument is `"any"`. The factory selects Vulkan when D3D12 is absent. There is no demonstrated need for a new `--gpu_backend` option. | `crackdown/src/host_tuning.cpp:137`, `sdk/src/ui/rex_app.cpp:317`, `sdk/include/rex/system/gpu_plugin.h:53`, `sdk/src/graphics/plugin_main.cpp:40` |
| Plugin loading | Linux plugin names are `librexgpu-<name><postfix>.so`. SDK target setup supplies `$ORIGIN` RPATH behavior. Distribution must still include the correct plugin and runtime shared libraries. | `sdk/src/system/gpu_plugin_loader.cpp:44`, `sdk/cmake/rexglue_helpers.cmake:91`, `sdk/cmake/rexglue_helpers.cmake:209` |
| Windowing | SDL3 windows; Linux surfaces support Wayland and X11/XCB. Windows native-window-handle access is separate from Linux surface creation. | `sdk/src/ui/window_sdl.cpp:139`, `sdk/src/ui/window_sdl.cpp:231`, `sdk/src/ui/window_sdl.cpp:402` |
| Input | SDL is the default input backend and constructs an SDL input driver. Controller discovery, reconnect, mappings, keyboard/mouse, focus, and relative mouse behavior still require Linux tests. | `sdk/src/input/input_system.cpp:27`, `sdk/src/input/input_system.cpp:337`, `sdk/src/ui/window_sdl.cpp:243` |
| Audio | The application selects `SDLAudioSystem`. Output uses SDL3 floating-point audio streams and folds output to stereo for devices with at most two channels. Linux audio-server support depends on the SDL build and host configuration. | `sdk/src/ui/rex_app.cpp:310`, `sdk/src/audio/sdl/sdl_audio_driver.cpp:46`, `sdk/src/audio/sdl/sdl_audio_driver.cpp:54`, `sdk/src/audio/sdl/sdl_audio_driver.cpp:76` |
| Guest filesystem | VFS mount/link matching and child lookup are case-insensitive. HostPathDevice additionally performs lazy case-insensitive directory lookup and retains actual host filenames. A case-sensitive Linux filesystem therefore does **not** inherently break `game:\` access. | `sdk/src/filesystem/virtual_file_system.cpp:64`, `sdk/src/filesystem/virtual_file_system.cpp:105`, `sdk/src/filesystem/entry.cpp:59`, `sdk/src/filesystem/devices/host_path_device.cpp:68` |
| Guest memory | There is a complete POSIX mapping implementation using shared backing objects, `mmap`, and `mprotect`. The guest arena includes the 4 GB virtual address space and physical aliases; backing size is approximately `0x120000000` plus granularity. This is virtual-address/backing-object size, not proof of equivalent resident RAM consumption. | `sdk/src/system/xmemory.cpp:140`, `sdk/src/system/xmemory.cpp:164`, `sdk/src/system/xmemory.cpp:178`, `sdk/src/system/xmemory.cpp:259`, `sdk/src/core/memory_posix.cpp:477`, `sdk/src/core/memory_posix.cpp:506` |
| Threads | POSIX threads, TLS, synchronization, suspend/resume signals, affinity, and priority handling exist. Linux guest suspend-count handling has its own implementation. Priority elevation can fail and produce warnings. | `sdk/src/core/threading_posix.cpp:102`, `sdk/src/core/threading_posix.cpp:231`, `sdk/src/core/threading_posix.cpp:651`, `sdk/src/core/threading_posix.cpp:815`, `sdk/src/core/threading_posix.cpp:838`, `sdk/src/system/xthread.cpp:941` |
| Clocks and timers | Host clock uses `CLOCK_MONOTONIC_RAW`. Sleep uses `nanosleep`, retrying after interruption. Timer and timer-queue implementations exist. Alertable sleep dispatches callbacks, but interruptible I/O remains a TODO. | `sdk/src/core/clock_posix.cpp:30`, `sdk/src/core/clock_posix.cpp:48`, `sdk/src/core/threading_posix.cpp:195`, `sdk/src/core/threading_posix.cpp:209`, `sdk/src/core/threading_posix.cpp:1314`, `sdk/src/core/timer_queue.cpp:30` |
| Exceptions and write watches | POSIX exception handling installs SIGSEGV/SIGILL handlers. The MMIO handler installs an exception callback for memory coherence/write tracking. Recompiled MMIO operations normally call `CheckLoad`/`CheckStore` through macros; they are not all serviced through SIGSEGV. | `sdk/src/core/exception_handler_posix.cpp:181`, `sdk/src/core/exception_handler_posix.cpp:422`, `sdk/src/system/mmio_handler.cpp:50`, `sdk/src/system/mmio_handler.cpp:371` |
| Other signal handling | A second POSIX SEH layer installs handlers for SIGSEGV/SIGBUS/SIGFPE/SIGILL. Its initialization and coexistence with write-watch handling deserve explicit tests. | `sdk/src/core/seh_posix.cpp:103`, `sdk/include/rex/platform/exceptions.h:90` |
| Known incomplete facilities | Native message boxes fall back to stderr; `EnableAffinityConfiguration()` is empty; alertable I/O is incomplete; thread serialization/restore is unimplemented; memory data providers and read watches are incomplete. These are not all Linux-specific blockers. | `sdk/src/core/system_posix.cpp:15`, `sdk/src/core/threading_posix.cpp:173`, `sdk/src/core/threading_posix.cpp:209`, `sdk/src/system/xthread.cpp:1309`, `sdk/src/system/xthread.cpp:1420`, `sdk/src/system/xmemory.cpp:2068`, `sdk/src/system/xmemory.cpp:2178` |

### Vulkan device requirements

The inspected device-selection code rejects a GPU-emulation device lacking:

- `independentBlend`.
- `fragmentStoresAndAtomics`.
- `vertexPipelineStoresAndAtomics`.
- `geometryShader` and `fillModeNonSolid` under the normal Linux defaults.
- A graphics-and-compute queue family.

Evidence: `sdk/src/ui/vulkan/vulkan_device.cpp:95`, `sdk/src/ui/vulkan/vulkan_device.cpp:112`, `sdk/src/ui/vulkan/vulkan_device.cpp:119`, `sdk/src/ui/vulkan/vulkan_device.cpp:126`, `sdk/src/ui/vulkan/vulkan_device.cpp:133`, `sdk/src/ui/vulkan/vulkan_device.cpp:403`.

Presentation additionally requires `VK_KHR_swapchain` and working surface/presentation support for the selected display system. Evidence: `sdk/src/ui/vulkan/vulkan_device.cpp:203`, `sdk/src/ui/vulkan/vulkan_device.cpp:285`, `sdk/src/ui/window_sdl.cpp:402`.

The source negotiates core versions and promoted extensions. **Do not equate the vendored Vulkan SDK version with the minimum runtime Vulkan version.** Dynamic rendering has a conditional path; requested extensions are enabled when available. Relevant capabilities include maintenance/memory-allocation extensions, shader float controls, stencil export, SPIR-V 1.4, custom border colors, and robustness2/null descriptors. Some affect rendering parity without being unconditional device-selection requirements. Evidence: `sdk/src/ui/vulkan/vulkan_device.cpp:153`, `sdk/src/ui/vulkan/vulkan_device.cpp:184`, `sdk/src/ui/vulkan/vulkan_device.cpp:213`, `sdk/src/ui/vulkan/vulkan_device.cpp:270`, `sdk/src/graphics/vulkan/command_processor.cpp:3096`.

Record actual device properties, extensions, format support, driver version, and validation results before publishing minimum GPU requirements. Static inspection does not establish a complete hardware compatibility matrix.

## 2. SDK patch disposition

Patch references in this table are relative to `patches/sdk/`.

| Patch | Linux/Vulkan disposition | Evidence |
|---|---|---|
| `0001-codegen-vpkd3d128-reads-a-copy-of-vB-when-vD-vB.patch` | Shared codegen fix for NPC heads. Keep it and regenerate affected code; no Vulkan equivalent is needed. | Patch `:22`, `:200` |
| `0002-ppc-bit-exact-vrsqrtefp-estimate-port-of-Xenia-s-tab.patch` | Shared PPC estimate/codegen fix associated with audio NaNs. Keep it and run instruction tests. | Patch `:31`, `:125`, `:144` |
| `0003-audio-XMA-context-guards-from-Xenia-for-stalls-on-hi.patch` | Shared XMA decoder/runtime guards. Keep it; Linux audio output still needs endurance testing. | Patch `:17` |
| `0004-xboxkrnl-silent-KeSave-RestoreFloatingPointState.patch` | Shared kernel stubs. No GPU port; retain their documented behavior. | Patch `:14` |
| `0005-d3d12-real-occlusion-query-results-BEGIN-END-pairing.patch` | Mixed scope: shared `OnWaitPoll` infrastructure plus D3D12 query behavior. Port the query behavior to Vulkan. | Patch `:47`, `:114`, `:147` |
| `0006-codegen-fused-vmaddfp-vnmsubfp-and-double-precision-vmsum.patch` | Shared codegen/intrinsics/build flags. Preserve fused semantics and the x86 FMA requirement. Do not replace it with weaker math to broaden CPU support. | Patch `:29`, `:45`, `:112`; `sdk/cmake/rexglue_helpers.cmake:60` |
| `0007-gpu-command-processor-fast-paths.patch` | Shared command-processor and shared-memory optimization. **Already applies to Vulkan**, whose bulk-register override already exists. Validate effects and speed; do not duplicate it. | Patch `:24`, `:59`; `sdk/src/graphics/vulkan/command_processor.cpp:2163`, `sdk/include/rex/graphics/vulkan/command_processor.h:252` |
| `0008-gpu-readback-resolve-max-bytes.patch` | D3D12 implementation despite generic cvar name. Move its cvar definition into shared GPU code and add the byte gate to Vulkan. | Patch `:18`, `:26`, `:39`; `sdk/src/graphics/vulkan/command_processor.cpp:4403` |
| `0009-utf8-path-cvars.patch` | Shared application path conversion. Keep it; validate UTF-8 paths on Linux. | Patch `:18`; `sdk/src/ui/rex_app.cpp:109`, `:132` |

### Occlusion-query work required

Vulkan currently requires matching BEGIN/END sample-count addresses. Patch 0005 documents that Crackdown uses different structures, with END’s address `0x20` before BEGIN’s. Vulkan also ends and discards an active query on submission completion, and its END path waits for all queue operations.

Evidence: `sdk/src/graphics/vulkan/command_processor.cpp:703`, `:4889`, `:5305`; `patches/sdk/0005-d3d12-real-occlusion-query-results-BEGIN-END-pairing.patch:10`.

Implement Vulkan equivalents for:

1. BEGIN snapshot zeroing and END pairing with the active guest query.
2. Legal host-query segments across rendering and submission boundaries.
3. Summing completed segments into END’s guest structure.
4. Pending-result processing through `OnWaitPoll`, including `WAIT_REG_MEM` progress.
5. Safe query-pool reuse, shutdown, fallback, and non-coherent readback invalidation.

The shared polling hook is already present: `sdk/include/rex/graphics/command_processor.h:171`, `sdk/src/graphics/command_processor.cpp:1056`. Vulkan currently provides explicit readback invalidation: `sdk/src/graphics/vulkan/command_processor.cpp:4896`.

Port semantics, not D3D12 command placement: Vulkan query/rendering-scope rules require separate review and validation.

### Sunlight and resolve readback

`readback_resolve=fast` already reaches Vulkan through the shared mode selector. It is required by the existing Crackdown defaults because CPU-visible resolved data fixes blown-out sunlight; that visual result has only been established on Windows.

Evidence: `sdk/src/graphics/command_processor.cpp:173`, `sdk/src/graphics/vulkan/command_processor.cpp:4393`, `crackdown/src/host_tuning.cpp:14`, `:94`.

Vulkan currently has no 0008 byte gate. Add it after the GPU resolve reports `written_length`, before allocating/copying readback resources. Oversized resolves must retain their normal GPU-side resolve. Keep zero meaning unlimited.

Evidence: `sdk/src/graphics/vulkan/command_processor.cpp:4408`, `:4414`, `:4427`; `patches/sdk/0008-gpu-readback-resolve-max-bytes.patch:12`.

First validate sunlight with unlimited readback, then validate `262144`. The Windows comparison is useful baseline evidence, not proof that Vulkan produces identical exposure data: `crackdown/src/host_tuning.cpp:21`.

## 3. Game and launcher portability audit

| Component | Current state and required action | Evidence |
|---|---|---|
| RenderDoc capture | F9 whole-guest-frame capture is Windows-only; Linux callback is empty. Add injected-library discovery through POSIX dynamic loading and use the existing SDL key-event path. | `crackdown/src/hooks_capture.cpp:31`, `:38`, `:76` |
| Precise frame sleep | High-resolution Windows waitable timer plus spin tail; Linux returns false and falls back to the guest/runtime sleep path. Measure Linux pacing before adding an absolute monotonic wait implementation. Preserve title-termination checks. | `crackdown/src/hooks_sleep.cpp:74`, `:89`, `:114`, `:167` |
| Simulation timers and frame logging | Uses `std::chrono::steady_clock`; no Windows API dependency was found in the inspected framerate/replay implementation. Existing frame CSV is usable on Linux. | `crackdown/src/hooks_framerate.cpp:95`, `:144`; `crackdown/src/hooks_input_replay.cpp:53` |
| 60 fps defaults | Shared implementation sets fast readback, 256 KB limit, vsync off, 60 fps cap, measured clock, literal step, and animation step. Explicit vsync true retains stock behavior. | `crackdown/src/host_tuning.cpp:93`, `:105` |
| Startup data discovery | Direct host probe checks lowercase `default.xex`, unlike the case-insensitive guest VFS. Fix discovery for supported original casing. Explicit `game_data_root` bypasses this probe. | `crackdown/src/host_tuning.cpp:150`, `:159` |
| PC options/XUI | Shared implementation and integrity/persistence tests already exist. Windows cache replacement uses `MoveFileExW`; Linux uses filesystem rename. Test writable cache roots and corruption recovery. | `crackdown/src/xui_patch.cpp:127`, `crackdown/CMakeLists.txt:87` |
| Event/storage/audio hooks | Retain the platform-neutral guest hooks. The storage hook’s “no HDD cache” behavior is still needed for streaming. The XAudio hook is a guest synchronization hook, not a requirement to implement Windows XAudio on Linux. | `crackdown/CMakeLists.txt:16`, `:17`, `:27`; `crackdown/src/hooks_storage.cpp:9` |
| AP networking | Bridge/state logic is shared, but `Client::Socket` is WinHTTP WebSocket transport, with secure connections delegated to WinHTTP. Linux explicitly reports unsupported transport. Implement bounded `ws`/`wss` transport with certificate verification, cancellation, and existing generation/lifetime semantics. | `crackdown/src/ap/client.cpp:47`, `:60`, `:70`, `:135`, `:241` |
| Launcher process/platform | Linux already has `posix_spawn`, working-directory actions, environment filtering, `waitpid`, flock locking, process identity, folder URLs, and XDG user-data selection. Extend and test these paths. | `crackdown/launcher/platform.cpp:40`, `:93`, `:165`, `:195`, `:241` |
| Installer | ISO/XISO parsing, extraction, hashes, staging, and final rename are shared. Linux symlink checks already exist. Validate case-sensitive manifests, cancellation, disk-full recovery, and data location. | `crackdown/launcher/core.cpp:56`, `:119`, `:190`, `:217`, `:232` |
| Launcher UI | SDL3/ImGui SDL renderer; Linux file-browser root exists. Some actions still use Windows executable names. | `crackdown/launcher/main.cpp:93`, `:104`, `:238`, `:384` |
| Updater | Network fetching, ZIP extraction, install/restart entry points are Windows-only. Shared validation/recovery also hardcodes `.exe`/`.dll` names and ZIP asset selection. | `crackdown/launcher/update.cpp:106`, `:130`, `:147`, `:211`, `:243`, `:295`, `:320`, `:335` |
| Steam/VDF | Binary VDF parsing/writing is shared. Discovery/process checking uses Windows registry/process APIs; Linux addition explicitly fails. Port discovery and executable paths while preserving backup/conflict checks. | `crackdown/launcher/steam.cpp:54`, `:57`, `:77`, `:85`, `:105` |
| System report | Linux already reports kernel and CPU, but lacks the Windows branch’s GPU/driver reporting. Add Vulkan device/driver details. | `crackdown/launcher/platform.cpp:204`, `:226` |
| Defender | No Defender/exclusion implementation was found in the inspected integration source or packaging scripts. Treat any future Defender functionality as Windows-specific; no Linux counterpart is needed. | Inspected Windows platform/action sites: `crackdown/launcher/platform.cpp:204`, `crackdown/launcher/main.cpp:236`, `tools/package-beta.ps1:62` |
| Packaging/diagnostics | Current beta packaging targets Windows binaries and scans PE DLL dependencies. Build/bench/capture wrappers include Windows-specific tools. Portable Python analysis tools can remain. | `tools/package-beta.ps1:74`, `:102`, `:152`; `tools/beta-README.txt:1` |

## 4. Staged implementation

### Stage A — reproducible build and boot to title

**Files to change**

- `crackdown/CMakePresets.json`: explicit Linux compiler override/documentation; current game preset requests Clang 20.
- `crackdown/src/host_tuning.cpp`: case-insensitive startup data discovery.
- Shared GPU cvar source and Vulkan command processor: register the byte-limit cvar once, implement its Vulkan gate.
- `patches/sdk/`: reviewable patch updates/additions.
- New Linux build/test wrapper and boot-smoke harness as needed.

Evidence: `crackdown/CMakePresets.json:24`, `sdk/CMakeLists.txt:21`, `tools/apply-sdk-patches.sh:3`.

**Implementation**

1. Pin the SDK revision, patch hashes, compiler, standard library, and generated-code provenance. Apply all nine patches and preserve independent reverse-application checks.
2. Rebuild/install the SDK with Vulkan enabled. Regenerate code affected by 0001/0002/0006; patching does not change the SDK version fingerprint used by generation stamps.
3. Stage the game with matching runtime/plugin libraries and XUI patches. Confirm ELF dependency resolution and actual Vulkan backend initialization.
4. Supply absolute game, user-data, and cache roots initially. Fix automatic case-sensitive discovery before calling the package portable.
5. Exercise mapping aliases, commit/protect, write-watch faults, thread suspend/resume, and clean termination before diagnosing every crash as a renderer problem.
6. Boot with AP disabled and normal intro skipping. Use synchronous shader compilation for initial diagnostic runs if asynchronous behavior obscures failures.

Evidence for generation invalidation and patch checks: `tools/apply-sdk-patches.sh:3`, `:20`. XUI staging: `crackdown/CMakeLists.txt:73`.

**Cvars**

- `gpu_plugin=xenos`.
- `readback_resolve=fast`.
- `readback_resolve_max_bytes=0` for the initial correctness baseline.
- Explicit `game_data_root`, `user_data_root`, `cache_root`.
- `vulkan_validation_enabled=true` on diagnostic runs.
- `vulkan_device=<index>` when selection is ambiguous.
- `vsync=true` for a stock-rate comparison; subsequently exercise the existing 60 fps defaults.

Do not weaken mandatory shader-store/atomic requirements merely to make a smoke test initialize.

**Tests and acceptance evidence**

- SDK unit/PPC suites with `REXGLUE_BUILD_TESTS=ON`.
- PC-options integrity and settings persistence tests.
- Launcher core tests with UI disabled.
- AP catalog/state tests, which do not configure networking.
- Mixed-case and UTF-8 dump/root paths on a case-sensitive filesystem.
- Repeat boot, title/menu navigation, controller interaction, and clean exit on a real Linux GPU.

Existing test entry points: `sdk/tests/unit/CMakeLists.txt:1`, `sdk/tests/ppc/CMakeLists.txt:1`, `crackdown/CMakeLists.txt:87`, `crackdown/launcher/CMakeLists.txt:22`, `crackdown/src/ap/CMakeLists.txt:1`.

Archive logs showing mapped memory, loaded plugin/backend, selected GPU/driver, title arrival, and exit. Save title screenshots and validation output.

**Headless server strategy**

Run builds and host tests on the server. Try lavapipe for Vulkan initialization and boot-to-first-frame smoke coverage if it meets required features.

The normal application still creates a window/presenter; **lavapipe plus SDL’s dummy driver is not an established working configuration**. Use a virtual display with working Vulkan WSI, or add a narrowly scoped smoke harness using the existing presentation-free provider path. Add bounded execution and an explicit first-frame/readback artifact.

Evidence: `sdk/src/graphics/vulkan/graphics_system.cpp:27`, `sdk/src/graphics/graphics_system.cpp:92`, `sdk/src/graphics/graphics_system.cpp:118`.

Software rendering is smoke coverage only. Playability acceptance requires a native Linux PC with a real GPU. Wine is outside this plan; Steam Deck support is optional follow-up.

### Stage B — gameplay and rendering parity

**Files to change**

- `sdk/include/rex/graphics/vulkan/command_processor.h`.
- `sdk/src/graphics/vulkan/command_processor.cpp`.
- Vulkan deferred-command-buffer code if segment/query operations require changes.
- New SDK patches and focused GPU regressions.
- `crackdown/src/hooks_capture.cpp`.
- `crackdown/src/ap/client.cpp` and game CMake dependencies if AP parity is included in the Linux release.

**Implementation**

Port 0005’s guest-query semantics and deferred completion to Vulkan. Finish Linux whole-guest-frame RenderDoc capture. Keep CPU/codegen and audio fixes intact.

Implement AP transport independently of the renderer. Until its transport tests pass, expose AP as unavailable on Linux instead of entering repeated unsupported connection attempts.

**Parity checklist**

| Check | Test and evidence |
|---|---|
| NPC heads | Inspect close/medium-distance NPCs, movement, and combat. Compare matched Windows/Linux captures; run 0001 instruction regressions. |
| Sunlight | Capture identical spawn/camera scenes after exposure settles. Compare unlimited readback with 256 KB, and use `none` only as a diagnostic control. Check outdoor, shadow, and indoor transitions. |
| Traffic-light glows | Compare unobstructed and building-occluded lights. Record BEGIN/END guest addresses, host segments, completion, and guest sample counts. Fake zero/constant counts are diagnostic controls, not fixes. |
| Audio | At least a 30-minute city/drive/combat session; music, voices, effects, stereo output, mute, device changes, and clean shutdown. Check NaNs, XMA stalls, underruns, and barrier hangs. |
| Loading/storage | New Game, streaming while driving, save/load, restart, and supported profile handling. |
| Simulation | Compare stock 30 fps and default 60 fps using replay: stairs/slopes, driving speed, impacts, car damage, and animation timing. |
| Input/options | Controller reconnect, focus changes, keyboard/mouse, fullscreen/resize, option persistence, corrupted XUI cache recovery, exit. |
| AP | Fake/local server plus verified TLS endpoint: connect, fragmented messages, disconnect/reconnect, stale generations, silent server, cancellation, receipts, persistence, and shutdown. |

**Cvars and tests**

Use `occlusion_query_enable=true`, fast resolve readback, and validated byte-limit defaults. Retain `sim_ground_snap_30hz=true`. Use existing `input_replay`, teleport, physics diagnostics, and `frame_log` for comparisons.

Add focused tests for different BEGIN/END addresses, rendering/submission splits, idle polling, query-pool exhaustion, non-coherent memory, ring-wrap bulk writes, and readback thresholds.

Evidence: `crackdown/src/hooks_framerate.cpp:78`, `:144`; `sdk/src/graphics/vulkan/command_processor.cpp:4811`, `:4896`; `sdk/src/graphics/command_processor.cpp:511`.

Acceptance requires matched screenshots/captures, traces, endurance logs, save/load results, and repeatable replay results. Follow CLAUDE.md’s evidence rule for every hook or patch: guest addresses, disassembly where applicable, and trace or screenshot validation.

### Stage C — launcher and tarball/AppImage distribution

**Files to change**

- `crackdown/launcher/{platform,main,core,steam,update}.cpp`.
- Launcher CMake and existing tests.
- `crackdown/src/host_tuning.cpp` path defaults.
- New Linux packaging scripts, AppRun, desktop entry, icon, and Linux README.

**Implementation**

1. Produce a relocatable tarball first, with native executables, matching `.so` dependencies, XUI patches, launcher assets, manifest, notices, and version metadata.
2. Separate immutable application files from writable game installation, profiles, cache, logs, and update staging. AppImage’s mounted application directory cannot serve as the current mutable package home.
3. Preserve user-supplied dump extraction and verification. Include no ISO, XEX, extracted assets, or generated source in git or public packages.
4. Generalize executable names and release assets by platform/architecture. Port HTTPS/download/archive handling with bounded extraction, hash verification, traversal/symlink rejection, cancellation, rollback, and executable permissions.
5. Give AppImage updates an external whole-image replacement workflow. Do not reuse Windows in-place `.exe` replacement assumptions.
6. Port optional Steam discovery/VDF integration for standard and sandboxed Linux installations; preserve backups and detect concurrent changes.
7. Add Vulkan driver/device details to system reports and redact AP secrets.

**Tests and evidence**

Run existing installer, process, VDF, hash, and update-recovery tests on Linux. Extend them for executable modes, read-only application roots, external data roots, interrupted updates, and platform-specific asset selection.

Test the packaged artifact from arbitrary working directories and paths containing spaces/UTF-8. Test fresh installation, cancel/retry, low disk space, profile persistence, relaunch, and update recovery. Verify ELF dependencies/RPATH on a clean Linux runtime.

Run the final tarball and AppImage on real hardware under both X11 and Wayland where available. Record supported glibc/runtime baseline, exact dependencies, GPU requirements, package hashes, and clean-machine results.

Cvars should retain Stage B’s validated rendering/simulation defaults; launcher path settings must point to writable external roots.

### Stage D — Linux performance

**Files to change**

- Linux profile preset only as needed.
- `crackdown/src/hooks_sleep.cpp` if measured pacing warrants a Linux precise wait.
- Renderer hot paths supported by Linux profiles.
- New Linux replay/benchmark wrappers; reuse `tools/frametimes.py`.

**Measurement**

Benchmark Release with `frame_log`; use the profile build for Tracy/perf counters and Linux `perf` where permitted. Guest frame CSV measures game-loop timing, so pair it with presentation/GPU timing when assessing visible stutter.

Evidence: `crackdown/src/hooks_framerate.cpp:144`, `crackdown/src/host_tuning.cpp:51`, `crackdown/CMakePresets.json:171`.

Measure idle spawn, camera pan, driving/streaming, and combat. Separate cold/warm shader caches. Record CPU/GPU/driver, governor, compositor/display mode, resolution, cvars, average frame time, tail latency, and query/readback costs.

Start at resolution scale 1 and the validated 60 fps settings. Compare unlimited/256 KB readback and uncapped runs before expanding to 1440p or higher targets.

If pacing overshoot is material, implement bounded absolute monotonic sleeping with interruption handling and title-termination checks. Tune any spin tail for CPU/power cost.

Patch 0007 already uses Vulkan’s bulk-register implementation; verify its benefit rather than assuming the Windows gain transfers unchanged.

**Acceptance**

Publish reproducible benchmark artifacts demonstrating sustained 60 fps on declared hardware, with acceptable frame-time tails. Re-run Stage B parity checks after changes affecting query scheduling, readback, synchronization, or simulation. Do not claim universal Linux performance.

## 5. Risks and unresolved questions

- **No Linux runtime evidence yet.** Static reading establishes implementation paths, not successful boot or playability.
- **Signal coexistence is a priority risk.** Two signal-handling layers exist. Confirm initialization order, handler chaining, write-watch recovery, and fatal-fault behavior before renderer debugging.
- **Guest mapping needs real execution.** Shared backing limits, container/shared-memory configuration, alias protection, fixed-address placement, and ASLR interactions may differ from Windows.
- **Vulkan occlusion queries require careful scope management.** Address pairing alone will not fix submission splits, query resets, completion, pool reuse, or GPU/CPU visibility.
- **Sunlight parity remains unverified.** Vulkan fast-readback timing/layout and the 256 KB threshold need visual and trace evidence.
- **Compiler and CPU baseline remain to be declared.** Successful Clang builds do not prove compatibility across standard libraries or CPUs; patch 0006 enables FMA on x86.
- **Audio stability remains unverified.** SDL backend availability, channel conversion, device changes, and long-running XMA synchronization require hardware testing.
- **Headless smoke feasibility is unknown.** Lavapipe may expose sufficient features, but window/WSI integration and acceptable boot time are unproven.
- **Packaging changes application assumptions.** External writable data roots and platform-specific updates are necessary for AppImage and installed/read-only deployments.
- **AP is currently nonfunctional on Linux.** Its state tests cannot validate the missing WebSocket/TLS transport.
- **Compatibility claims must follow testing.** Minimum GPU/driver versions, distro/runtime baseline, and X11/Wayland coverage cannot be established from this audit alone.