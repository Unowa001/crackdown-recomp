# Crackdown recomp

Static recompilation of Crackdown (Xbox 360, 2007) to native PC, built on the ReXGlue SDK (`sdk/`, pinned to a release tag).

## Hard rules

- **No game data or generated code in git.** The ISO, XEX, extracted files and generated C++ stay local (see `.gitignore`). Users supply their own dump.
- **Evidence rule.** Every hook or patch commit cites: the guest address(es) touched, the disassembly it was based on, and the emulator (Xenia) trace or a screenshot comparison it was checked against. No evidence, no merge.
- **SkiddyToast/Crackdown is reference only.** It has no license, so read it to learn from it, but never copy code from it.
- **Build host:** this server (Clang 18+, CMake 3.25+, Ninja). **Run/test host:** Windows PC (RTX 5080). This server is headless.

## Agents (see the `/multi-agent-review` skill)

- **Claude Code:** the only writer. Handles hooks, runtime/renderer patches, builds and merges.
- **agy (Gemini, read-only):** sweeps the generated C++, crash logs and Xenia trace diffs; summarizes Xenia source.
- **Codex (`-s read-only`):** reviews every hook/patch diff before merge; gives a second opinion on PPC/VMX128 semantics.
- Receipts on commits name every agent that produced or checked the change.

## Long-term goal: randomizer

After the port, build a Crackdown randomizer (orbs, pickups, weapon/vehicle spawns, etc.). While porting:
- When you find a function or data table for pickups, orbs, spawns or the item/weapon tables, record its guest address and layout in `docs/re-notes.md` with evidence.
- Prefer hooks that intercept data *loading* (where we can substitute data) over hardcoded patches.

## Local setup

- Reference dump: `game/Default.xex`, sha256 `7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a` (World multi-language disc, 197 files).
- Extract with `~/tools/extract-xiso/build/extract-xiso -d game -x <iso>`. 7-Zip can't read XDVDFS.
- SDK presets call unversioned `clang`. Pass `-DCMAKE_C_COMPILER=clang-NN -DCMAKE_CXX_COMPILER=clang++-NN`.

## Status (2026-10-11)

- Codegen is clean (0 unresolved calls). The Linux build links. The server is headless, so it can't test the game.
- **The game is playable on Windows:** title screen, menus and New Game into the city all work (xenos GPU, controller input).
- Two hooks were needed to get past the New Game loading screen: `crackdown/src/hooks_events.cpp` (0x82A4FA70, an inline event reset the runtime can't see) and `crackdown/src/hooks_storage.cpp` (0x827457E8, which reports "no HDD cache" so everything streams from `game:\`). Details are in `docs/playbook.md` (runtime-gaps table, section 7) and `docs/re-notes.md`. Both have a Xenia source comparison and a Codex review, and passed a PC New Game run on 2026-10-10 (about 3 min in the city, no hang, clean exit; log `crackdown_009`).
- **SDK fixes live in `patches/sdk/000N-*.patch`** (applied by `build-and-run.bat` / `tools/apply-sdk-patches.sh`; see playbook section 2), not as commits in the submodule. 0001 NPC heads (vpkd3d128 in place), 0002 audio NaN (vrsqrtefp estimate), 0003 XMA guards, 0004 silent FP-state stubs, 0005 occlusion queries (glows through walls), 0006 fused vmaddfp + double-precision vmsum dot products (slope/stairs speed build-up; needs an FMA3 CPU), 0007 GPU command processor fast paths (perf), 0008 `readback_resolve_max_bytes` (read back only small resolves; game default 262144). All verified on the PC (0001-0005 on 2026-10-11, 0006-0008 on 2026-10-12). Every patch must reverse-apply on its own with the whole stack in (the scripts check); a patch must never edit lines another patch adds.
- **Fixed 2026-10-11:** NPC heads render, audio no longer dies (NaN) or stalls (XAudio barrier hook `hooks_xaudio.cpp`, 0x82A50000; v2 still needs one 10+ min run), traffic-light glows no longer show through buildings. Boot logos are skipped (`hooks_intro.cpp`). F9 under RenderDoc captures two whole guest frames (F11 toggles fullscreen) (`hooks_capture.cpp`).
- **Blown-out sunlight fixed (2026-10-11):** the game needs `readback_resolve=fast` (CPU reads back a resolved texture, apparently scene brightness). `host_tuning.cpp` `ApplyGameDefaults()` sets it unless the user did. No known rendering bugs right now. Xenia Canary for reference is in `C:\tools\xenia` (its compatibility report: xenia-project/game-compatibility#148).
- **After an SDK GPU change, copy the DLL** (`rexgpu-xenos[rd].dll` from `sdk/out/install/win-amd64/bin`) or delete the exe: it is only copied when the exe relinks.
- **Windows build env:** use the x64 environment (`vcvars64.bat`; the plain Developer prompt is x86 and fails with `machine type x86 conflicts with x64`) and put `%ProgramFiles%\LLVM\bin` on PATH, like `build-and-run.bat` does.
- Logs: the runtime writes `crackdown/out/build/<preset>/logs/crackdown_NNN.log`, which is more reliable than the tee'd `run.log`. `--perf_log_csv` only writes rows in non-Release builds (perf counters are compiled out of Release; the file gets just a header), so measure the release exe with PresentMon (`C:\tools\PresentMon.exe`).
- **Perf (2026-10-12):** the GPU command processor thread is the bottleneck (~8,700 draws/frame at the spawn point). Patch 0007 took the spawn-point stand from 51 to 60 fps (67 uncapped, profile build). Unattended bench: `crackdown/bench/stand60.rec` + `--perf_log_csv` on the profile exe; poor-man's sampler = repeated `cdb -pv` stack dumps (PresentMon and ETW sampling need admin).
- **Perf done for beta (2026-10-12):** Release exe, uncapped: spawn camera pan 91-94 fps avg, 1% low 13.8-14.1 ms; driving 74-82 fps (before 0008). Bench the Release exe with `--frame_log=<csv>` + `tools/frametimes.py`. 1440p (`resolution_scale=2`) is post-beta.
- **Perf toward 1.0 (2026-10-17):** after 0007/0008 the CP thread was still 100% busy at 1x and 2x (2x costs almost nothing; 3x/4K is GPU-bound, 97%). SDK patch 0021 moves D3D12 command-list recording/execution to a "D3D12 Submission" thread (`--d3d12_async_submission`, default on; `--d3d12_async_submission_draws=2000` splits submissions; `--d3d12_async_submission_stats` logs subs/frame and swap wait): +10-30% avg fps, 1440p driving now GPU-bound. SDK patch 0022 checks shared-memory validity without the lock and narrows CPU-write invalidation to 16 pages (`--gpu_invalidation_expand_pages`, `--shared_memory_stats`): another +12% stand, +31% pan at 1x. `ApplyGameDefaults()` now sets `clear_memory_page_state=false` (SDK default true re-uploaded ~20 MB/frame; now ~2 MB): pan 1x 111 -> 130 fps. A city street (`--input_replay=bench/stand60.rec --teleport=1000,80,-1000`) is the new worst case and standard bench: ~54 fps at 1x, GPU ~41%, CP busy mostly with register writes, guest waiting in sub_826BA678. SDK patch 0023 copies register ranges without side effects in one go (`--gpu_fast_register_ranges`, default on): street 56 -> 62-63 fps, spawn 94 -> 108, pan +17-34% at 1x. (An earlier 0023, skipping unchanged constant writes, was neutral and dropped.) SDK patch 0024 skips repeated per-draw render target ownership walks, empty transfer passes and sampler recomputation (`--gpu_draw_state_memo`, default on): spawn +12-14%, street ~+3%. Next CP hotspot: UpdateBindings texture/SRV work and TextureCache::RequestTextures. Patch 0025 is reserved for the DLC thread. SDK patch 0026 keeps the GPU command processor and D3D12 submission threads off logical CPUs 0/1, where Windows handles most interrupts (`--gpu_host_threads_avoid_cpu0`, default on): street 66 -> 70 fps, 1% low 22 -> 18 ms, slow frames over 20 ms ~62 -> ~2 per 30 s. Next perf patch is 0027. Bench numbers: `docs/playbook.md` section 8 results log.
- **Plans:** `docs/plan-launcher-installer.md` (next), `docs/plan-randomizer.md` (Archipelago). Codex may implement scoped pieces on its own `wip/*` branch; Claude Code reviews, tests on the PC and merges.
- **60 fps is the default (2026-10-12):** `ApplyGameDefaults()` sets `--vsync=false --fps_cap=60 --sim_game_clock --sim_step_literal --sim_anim_step` unless the user set them (`--vsync=true` gives stock 30 fps back). The stairs speed bug is fixed by `--sim_ground_snap_30hz`. Driving and car damage match 30 fps. Frame-locked leftovers are in `docs/60fps-audit.md`. Unattended gameplay tests: `--input_replay`, `--teleport`, `--teleport_vehicle`, `--diag_char_proxy`, `--diag_vehicle` (recordings in the gitignored `crackdown/bench/`).
- **Roadmap (user, 2026-10-12):** performance (60 fps at 1080p/1440p first; ideally 4K/120 later), then a full installer / ISO extractor / launcher, then in-game QoL (exit button, graphics options, etc.), then the randomizer. Lighting is done.
- Be economical with compute: grep/tail logs; send bulk reading to agy or Codex (only on the server, where they're installed).
