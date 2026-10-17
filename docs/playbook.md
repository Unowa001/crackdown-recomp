# Xbox 360 recomp playbook

A step-by-step guide that isn't tied to one game. It records what we did for Crackdown, why each step exists, and what went wrong, so the next game starts here.
Game-specific findings go in `re-notes.md`.

## 0. Mental model

- A 360 game is one PowerPC executable (`default.xex`) plus data files. **Static recompilation** converts every PPC function into C++ *ahead of time*, then compiles that with Clang into a native exe. No emulation of the CPU at runtime, unlike Xenia's JIT.
- The CPU is the easy part. Everything else the game touched (kernel calls, the Xenos GPU, XMA audio, controllers) has to be supplied by a **runtime**. ReXGlue ships one derived from Xenia.
- So the work has three layers:
  1. **Codegen correctness:** find every function, where it starts and ends, and every switch table. The analyzer guesses, and we fix its guesses in the TOML config.
  2. **Runtime gaps:** kernel imports the game uses that the runtime doesn't implement yet.
  3. **Port quality:** renderer fixes, frame-rate unlock, widescreen, keyboard and mouse, settings, installer. This is where most of the time goes.

## 1. Get the binary out of your disc

- The ISO is XDVDFS (Xbox's own filesystem). 7-Zip only sees the small video partition and fails. Use `extract-xiso -x`; build it from github.com/XboxDev/extract-xiso.
- Before deleting the source archive, check the ISO's CRC against the archive's stored CRC (`unzip -v`).
- Record the sha256 of `default.xex`. Every address in your notes is only valid for that exact binary.
- Keep game data and generated code out of git. A recomp repo ships only *our* code, and users supply their own disc.

## 2. Build the SDK (once per machine and SDK version)

- Needs: Clang (the wiki says 20+ on Linux, though CMake only enforces 18), CMake 3.25+, Ninja, `libx11-xcb-dev`, `libvulkan-dev`.
- The SDK presets call unversioned `clang`. On Ubuntu, pass `-DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20`.
- `cmake --preset linux-amd64 ...` then `cmake --build out/build/linux-amd64 --config Release --target install`. The install step registers the SDK so projects find it with `find_package(rexglue)`.
- Pin the SDK to a release tag (git submodule). Its API breaks between versions.
- **SDK fixes that aren't upstream yet** live as `patches/sdk/000N-*.patch` (made with `git format-patch` against the pinned tag), never as commits inside the submodule. `build-and-run.bat` applies them before the SDK build; on Linux run `tools/apply-sdk-patches.sh`. Both skip patches that already reverse-apply. After a patch is newly applied, rebuild + install the SDK **and** delete `crackdown/generated/default/codegen*.stamp`: the codegen stamp fingerprints only the SDK version string, so a codegen fix would otherwise not regenerate anything. SDK DLLs (the `rexgpu-xenos[rd].dll` GPU plugin) reach the game folder only through a POST_BUILD copy when the exe relinks, so a GPU-only SDK change runs the **old** DLL until you delete the exe or copy the DLL from `sdk/out/install/<preset>/bin`. When bumping the SDK, drop the patches that landed upstream and re-check the rest. `.gitattributes` keeps `*.patch` byte-exact (`-text`); with `autocrlf` a CRLF copy stops applying.

## 3. How codegen finds functions (why it gets things wrong)

The analysis runs seven stages. Each one is a place where the analyzer's guesses can be wrong:

| Stage | What it does | Typical failure, and the fix in the TOML |
|---|---|---|
| Register | Seeds function starts from imports, compiler helpers, `.pdata` (the exception table lists most function starts and sizes), and the XEX entry point | Not much goes wrong here |
| Scan | Splits sections into code and data by runs of null words | Data embedded in code gets treated as instructions → `[[invalid_instructions]]` |
| Discover | Follows branches from each known start; `bl` targets become new functions; reads vtables via RTTI | Indirect calls (function pointers) aren't followed, so functions get missed → `[functions]` |
| GapFill | Code not reached from anything becomes a speculative function | A wrong boundary → `[functions]` with `size` or `end` |
| Merge | Resolves jumps and seals function boundaries | A tail call into another function → `parent` chunk |
| Validate | Every call target must resolve | Lists **unresolved calls**. Expected on the first run; generate anyway with `--force` |

- **Switch tables** (`bctr`): detected by pattern. When that fails, add `[[switch_tables]]` with the `bctr` address, the index register and the target labels.
- **Authority:** functions you list in the config always beat the analyzer's own guesses, so the config is the source of truth.

## 4. Changing behavior without touching generated code

- **Function overrides** (`REX_HOOK`): replace a whole guest function with native C++. Generated functions are weak symbols, so ours wins at link time.
- **Mid-ASM hooks** (`[[midasm_hook]]`): call our C++ at a single instruction, with access to registers, and optionally return or jump. Good for small fixes, like changing a value right after the game loads it.
- **ReXCRT:** map the game's CRT functions (memcpy, heap, file I/O) to native versions. This is faster, and it's a free way to see every file the game opens.

## 5. First codegen pass (what actually happened on Crackdown)

- `rexglue init --project-name X --xex-path game/Default.xex --game-root game --project-root ./X`. Paths are relative to the **cwd**, and the manifest gets them rewritten relative to the project. The wiki's `--app_name` flags are outdated in v0.9.0, so trust `--help`.
- `init` **lowercases** the XEX path. On Linux, fix the manifest so it matches the real file case.
- Put corrections in `config/functions.toml` and point the manifest's `includes = [...]` at it. Comment every entry with its evidence.
- Run `rexglue codegen <manifest>` *without* `--force` first. It lists `UnresolvedCall` errors. `rexglue --force codegen` (global flag, before the subcommand) emits code anyway, so you can read the disassembly in the comments of the generated `.cpp`.
- Then fix and rerun until there are 0 errors, **and** grep the output for `REX_FATAL` / `PPC_UNIMPLEMENTED`. The recompile stage can warn even after validation passes.

### Patterns seen so far

| Symptom | Cause | Fix |
|---|---|---|
| `b X - target not in any function`, and the source is `addi r3,r3,-N; b X` | A C++ this-adjustor thunk chain. The target thunk is only reachable via `b`, so it sits unclaimed between functions | `0xX = {}` |
| A recompile warning that a `b X` lands mid-function, where two tiny wrappers set a register and then jump to a shared body | The analyzer folded the shared body into the second wrapper | `0xWrapper = { size = 8 }` plus `0xX = {}` |

| Runtime `[FATAL] Call to invalid or unregistered function at 0xX` | A function reached only through a function pointer (vtable or callback), invisible to static discovery | Gap scan (below) |

### Gap scan: find every missed function at once (instead of one crash per run)

**Script: `tools/gapscan.sh <project>`.** It runs rounds until no new gaps appear (capped at 6, about 2 min each). Steps:

1. From the generated `.cpp`, compute each function's end as start + 4 × (number of `// instr` comment lines). Every gap of 16 bytes or more before the next function is a candidate.
2. Register all candidates in `config/gapfill.toml` and rerun codegen. Expect many new "unresolved branch" errors: those come from gaps that were really the middle of an existing function (the instruction count undercounts there).
3. Drop every entry whose range contains an address named in an error, and repeat until there are 0 errors.
4. Also drop entries whose preceding function doesn't end in `blr`/`bctr`/`b`. After a `bl`, the code may just continue.
4b. Then check that every entry got a `DEFINE_REX_FUNC` body. One that didn't (Crackdown: 0x82B67024, a 4 KB "gap") is the **import-thunk table**. Overriding it breaks every kernel call (a link error: undefined `sub_…`).
- **Gaps can hold several functions back to back.** Registering the first one shows the next (Crackdown: 0x82609B10 had a 24-byte stub followed by a 16-byte stub at 0x82609B28, which became the second crash). So the scan repeats, with an **8-byte** threshold, because an `addi`+`b` thunk is only 8 bytes.
- Crackdown second run: 6 rounds took entries 543 → 965 → 1116 → 1216 → 1295 → 1364 → 1431, still not fully converged, with 222 rejected and 0 unresolved.
5. Crackdown first pass: 720 candidates, then 555 after error pruning, then 544 after the `bl` filter, then 543 after the thunk check. Clean codegen and link. The crash address was among them.

Crackdown result: 9 config entries, then a clean codegen (about 30s, 182 MB of C++ across 93 files).

## Log of gotchas

- (Crackdown) `Default.xex` has a capital D. Our `*.xex` ignore rule is case-sensitive, but `game/` is ignored as a whole, so it doesn't matter.
- The runtime opens `default.xex` in lowercase. Windows doesn't care about case. On Linux, add `ln -s Default.xex game/default.xex`.
- The exe needs `--game_data_root=<path to game>`. On Linux it also needs `LD_LIBRARY_PATH=<sdk install>/lib`.
- A headless server can't smoke-test: SDL fails with "x11 not available", and `SDL_VIDEODRIVER=offscreen` just hangs silently. Use a real desktop.
- The runtime writes its own rotating log next to the exe: `out\build\<preset>\logs\<app>_NNN.log` (5 MB × 20, with `.1` holding the older half). That log exists even when the tee'd `run.log` is missing.
- (Crackdown) `KeSave/RestoreFloatingPointState STUB` spam (about 225 lines/s) came from the guest GPU interrupt callback running on the host `GPU Commands`/`GPU VSync` threads. It starts at the title screen and **doesn't** show that the game loop is alive. SDK patch 0004 makes both silent `STATUS_SUCCESS` (callers ignore the result).
- At the default `info` level, the log shows only *failed* file opens. Successful I/O is a `log_noisy` trace, and `KeWaitForSingleObject` logging is commented out. For a hang, take thread stacks first (section 7) and only then turn on noisy logging.
- (Crackdown) A failed `cache:\…` open is expected. The SDK leaves `cache:` unmounted on purpose (`runtime.cpp` SetupVfs). A trailing `c` on a file name (`globalc`, `.resblockc`) is the compressed variant, and the game falls back to the plain file. Not every failed `cache:` open is harmless, though: see the second row of the table below.

### Runtime gaps found at run time

| Symptom | Cause | Fix |
|---|---|---|
| (Crackdown) New Game hangs on the loading screen. No crash, and one guest thread spins in `KeWaitForMultipleObjects` | The game resets an event **inline** (`stw 0,4(r3)` into `SignalState`, at 0x82A4FA70). The runtime waits on a host `XEvent` that shadows the guest KEVENT and never sees that write (Xenia does: its `GetNativeObject` calls `XEvent::SyncFromGuest`, which the SDK dropped), so the event stays signaled and the job dispatcher (sub_8225C538) never gets `WAIT_TIMEOUT` and never starts the queued job | `REX_HOOK_RAW(sub_82A4FA70)` keeps the store and also calls `__imp__KeResetEvent`, **but only if `+8` holds the `REX\0` shadow signature and the type byte is ≤ 1** (`crackdown/src/hooks_events.cpp`). The linker folded identical bodies, so the same address is also a C++ vtable method that gets ordinary objects; an unguarded kernel call there hits `assert_always`. Before hooking a tiny function, search the decoded image for its address as a literal (vtables), not only for `bl`. To find others like it, look for tiny functions that write to offset 4 of a dispatcher object without calling `Ke*` |
| (Crackdown) After that fix, New Game still hangs. A trace shows an endless `NtReadFile(handle=0xFFFFFFFF)` → `0xC0000008` | The game's statically linked `XMountUtilityDrive` (0x827457E8) "succeeds": the SDK's NullDevice accepts the raw cache-partition I/O and the `Stfs*` stubs return SUCCESS, but `\Device\cache0` never exists. The game enables its HDD cache, the boot-time copy to `cache:\…c` fails, and New Game reads through a handle that never opened | `REX_HOOK_RAW(sub_827457E8)` returns `ERROR_DEVICE_NOT_CONNECTED` (a workaround choice: the caller only checks for nonzero, and what a real drive-less 360 returns is unknown), so everything streams from `game:\`. Xenia instead mounts `cache:` as a host folder by default (`mount_cache=true`) (`crackdown/src/hooks_storage.cpp`). Other games that call XMountUtilityDrive likely hit the same problem in this SDK. Look for the `"Josh"` magic (`0x4A6F7368`) next to `StfsCreateDevice` |
| (Crackdown) NPC heads missing (hair tufts render, faces don't) | SDK codegen bug: `vpkd3d128` writes vD piecewise while still reading vB, so an **in-place** pack (`vpkd3d128 v0,v0,5,2,2`, ~130 sites, mostly sub_826012D0 / sub_82719CA0) loses the float16 x sign. The head mesh is CPU-packed to float16 every frame, so heads were drawn ~600 px off screen. Found with a RenderDoc capture: head draw has all VB x/y/z ≥ 0 | SDK patch 0001 (read a copy when vD == vB). General lesson: for any VMX builder that writes vD in pieces, test vD == vA/vB |
| (Crackdown) Audio dies with a pop and stays silent (front L/R = NaN forever) | SDK `vrsqrtefp` was an exact 1/sqrt. The game's Newton step from the real 12-bit estimate always lands just below 1/sqrt; from the exact value it can round above, a normalized dot product reaches 1.0000001, and the arccos approximation (sub_8225F910, from 3D sound sub_82252150) returns NaN, which poisons the mix | SDK patch 0002: bit-exact port of Xenia Canary's estimate table. When a float bug shows in our build but not in Xenia, diff the SDK builder against Xenia's `x64_backend.cc` / `x64_seq_*.cc` helpers |
| (Crackdown) Audio stops silently after minutes; no NaN, game keeps running | XAudio's mix-worker barrier (sub_82A50000) assumes each worker owns a hardware thread. A worker preempted between marking its byte and its first read misses the release, marks into the next round and spins forever; the audio callback sub_82A51150 waits on the workers with no timeout. Found with live cdb stacks: two workers spinning at loc_82A500DC | `REX_HOOK_RAW(sub_82A50000)` with a host-side round counter (`crackdown/src/hooks_xaudio.cpp`, in testing). Any title with a spin barrier keyed on KPCR cpu number can hit this |
| (Crackdown) Running down slopes/stairs builds up a huge speed that carries on afterwards (stock 30 fps too; not in Xenia Canary) | SDK codegen precision: `vmaddfp`/`vnmsubfp` (+128 forms, ~4.9k sites) were a separate multiply and add, but the hardware and Xenia fuse them; `vmsum3fp128`/`vmsum4fp128` (~4.8k sites) used `dpps`, which rounds after every step, while Xenia multiplies and sums in double and rounds once. Last-bit differences in the movement math accumulated. Found by diffing the SDK float builders against Xenia Canary `x64_sequences.cc` | `patches/sdk/0006-codegen-fused-vmaddfp-vnmsubfp-and-double-precision-vmsum.patch` (helpers in `intrinsics.h`; guest code builds with `-mfma -ffp-contract=off`, so an FMA3 CPU is required). PC stock run 2026-10-12 `crackdown_023`: stairs clean |
| (Crackdown) Traffic-light glows (coronas) show through buildings | The SDK's D3D12 occlusion queries never delivered a result: BEGIN and END use **different** sample-count structs (END = BEGIN − 0x20; the result is END − BEGIN), but the handler required one address, so every END fell back to the fake count (1000 = fully visible). The result was also read before the GPU had run the query, and a query crossing a submission boundary switched host queries off for good | SDK patch 0005: pair END with the active query, write results when the submission completes (FIFO, polled in the CP idle loop), one host query per submission summed. Quick triage for any "visible through walls" glow or flare: `--query_occlusion_fake_sample_count=0` hides them all. `tools/diag/occlusion-diag.diff` adds an F10-triggered log of 400 query events |
| (Crackdown) Surfaces in direct sunlight blown out to near white; shadowed areas fine (Xenia Canary shows it too) | The game reads a GPU-resolved texture back on the CPU (apparently scene brightness for exposure/bloom); the SDK's `readback_resolve` defaults to `none`, so the CPU never sees it | `readback_resolve=fast` (previous frame's copy, no measurable cost at 30 fps), set as a game default in `host_tuning.cpp` `ApplyGameDefaults()`. **Check the title's Xenia compatibility report first**: it named this fix. For any game-specific SDK setting, use `ApplyGameDefaults()` so the command line can still override it |

## 6. Building and running on Windows

1. Install VS 2022 with "Desktop development with C++" (it provides the Windows SDK, CMake and Ninja), plus Clang 20+ via `winget install LLVM.LLVM`. VS 2022 bundles an older Clang. `vcvars64` does not put any Clang on PATH, so `build-and-run.bat` adds `%ProgramFiles%\LLVM\bin` itself.
1b. Turn on **Developer Mode** (Settings > System > For developers). The SDK's libmspack and o1heap dependencies contain git symlinks, and without Developer Mode they check out as one-line text files ("lzxd.c:1: expected identifier"). `build-and-run.bat` re-checks them out as real links.
2. Open an **x64 Native Tools Command Prompt for VS 2022**.
3. `git clone --recursive https://github.com/Unowa001/crackdown-recomp`, then copy the `game\` folder in next to `crackdown\`. The Samba share works, or use extract-xiso on Windows.
4. Build the SDK (after applying `patches\sdk\*.patch`, see section 2; `build-and-run.bat` does both): `cd sdk && cmake --preset win-amd64 && cmake --build out\build\win-amd64 --config Release --target install`
5. Codegen: `cd ..\crackdown && ..\sdk\out\install\win-amd64\bin\rexglue.exe codegen crackdown_manifest.toml`
6. Build: `cmake --preset win-amd64-release && cmake --build --preset win-amd64-release`
6b. The GPU is a plugin. Add `rexglue_setup_target(<name> GPU_PLUGINS xenos)` in CMakeLists **and** pass `--gpu_plugin=xenos` at launch. Without both, the log says "no GPU emulation loaded".
7. Run: `out\build\win-amd64-release\crackdown.exe --game_data_root=..\game --gpu_plugin=xenos` from inside `crackdown\`

## 7. Debugging a hang on Windows

1. **Symbols without a recompile:** lld-link can emit a PDB of public symbols from a relink alone: `cmake -B out\build\win-amd64-release -DCMAKE_EXE_LINKER_FLAGS=-Wl,/DEBUG:FULL`, then build (only the link step runs). Generated functions are external symbols, so frames show as `crackdown!_imp__sub_XXXXXXXX+off`, where `XXXXXXXX` is the guest address.
2. **Debugger:** `winget install Microsoft.WinDbg`. `cdb.exe` is under `(Get-AppxPackage Microsoft.WinDbg).InstallLocation\amd64\`.
3. **Stacks while it's hung (non-invasive, the game keeps running after `qd`):** `cdb -pv -p <pid> -y <build dir> -c "~*k 40; .dump /ma <scratch>\hang.dmp; qd"`. Guest threads are named `XThreadNNNN`, and the guest main thread is `Main XThread`.
4. **Find the spinner:** run `!runaway 3` twice, a few seconds apart. A thread that gains CPU time while sitting in a "wait" frame is in a wait that keeps returning immediately.
5. **Read guest memory from the dump:** the guest base is host `0x1'00000000` (check: `db 0x1'82000000` shows `MZ`). The data is big-endian. Take addresses from the `lis`/`addi` pairs in the function's disassembly comments (`lis rX,-32045` = 0x82D30000).
6. **Guest dispatcher objects:** a KEVENT the runtime has seen holds `0x52455800` ("REX\0") at +8 and the host handle at +12. `SignalState` at +4 is the *guest* view only.
7. **Grepping generated code:** `generated/` is gitignored, so the Grep tool and plain `rg` skip it. Use `rg --no-ignore`.
8. **Trace logging for a hang:** `--log_level=trace --log_noisy=true --log_max_file_size_mb=100 --log_max_files=40`. During a hot loop this writes about 25 MB/s, so the 40-file rotation keeps only about 3 minutes. Start the trace run right before reproducing, then grep with `rg` and never load the files whole (`Get-Content` on GBs of log got the shell killed). A loop of identical calls with the same failing status (here `NtReadFile` → `0xC0000008`) is the signal. Match its arguments (length, offset) against fields in the stuck object from the dump.
9. **Follow the handle back:** take the handle the loop uses (`-1`), find the `NtCreateFile` that should have produced it, and check which thread tried to *write* that file earlier. That's how the HDD-cache copy turned up.

## 8. Benchmarking (one change at a time, decided on a number)

**Profiling build.** The SDK compiles its Tracy zones (`REXGLUE_ENABLE_PROFILING`) and per-frame counters (`REXGLUE_ENABLE_PERF_COUNTERS`) into every config *except* Release, and exports those defines to the game the same way. So the `*-profile` preset is RelWithDebInfo with the flags overridden back to `-O3 -DNDEBUG`, and on Windows `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=""` (CMake's Windows Clang otherwise adds `-g -gcodeview` for RelWithDebInfo). The PDB comes from `/DEBUG:FULL` at link. The SDK's RelWithDebInfo libraries carry an `rd` suffix, so they install next to the Release ones.

1. SDK, once per SDK version: `cd sdk && cmake --build out\build\win-amd64 --config RelWithDebInfo --target install && cd ..`. This uses the SDK's own RelWithDebInfo flags (`-O2 -g`). A/B pairs are always built the same way, so it doesn't matter.
2. Game: `cd crackdown && cmake --preset win-amd64-profile && cmake --build --preset win-amd64-profile`.
3. Run, from `crackdown\`: `out\build\win-amd64-profile\crackdown.exe --game_data_root=..\game --gpu_plugin=xenos --perf_log_csv=<phase>.csv`.

**The runs.** Do **one run per phase**. The CSV has no markers, so each window is cut from the end of the file.
- Every run: New Game, skip intros, and wait at the spawn point until the HUD is up and streaming settles (about 20 s).
- **stand.csv:** don't touch the controller for at least 35 s, then press Alt+F4 at once.
- **drive.csv:** get into the nearest car, drive straight along the road for at least 35 s, then press Alt+F4 at once.
- `python ..\tools\frametimes.py stand.csv` (and the same for `drive.csv`) reports the 30 s before the last 2 s: frame count, average, 1% low and max. 1% low is the mean of the slowest `max(1, n // 100)` frames, in ms.
- `frame_time_us` is the host time between guest `XE_SWAP` packets, so a 30 fps cap shows up as 33.3 ms. Compare frame time against the cap, and use Tracy busy time to see the headroom.

**Tracy (CPU).** The profiler GUI must match the client: **v0.13.1** (release zip from github.com/wolfpld/tracy).
- Start the game **as administrator**, so Tracy gets ETW context switches.
- The client is on-demand. Connect when the phase *starts* and stop the capture when it ends, because the CPU data window (per-thread running time) covers the whole capture, not a selected range.
- Read: CPU data (running time per thread), and the top zones by self time.
- There are **no GPU timestamp zones** in the SDK (`SCOPE_profile_gpu_*` are empty, `rex/dbg.h`). Zones named `gpu` are CPU time on the command-processor thread.

**GPU time:** run PresentMon (github.com/GameTechDev/PresentMon, the console build) alongside, as administrator: `PresentMon --process_name crackdown.exe --output_file pm_<phase>.csv --timed 30 --terminate_after_timed`, started at the beginning of the phase. Read the average of the GPU busy column.

**Results log:**

| Date | Change | Stand avg / 1% low (ms) | Drive avg / 1% low (ms) | GPU busy (ms) | Verdict |
|---|---|---|---|---|---|
| 2026-10-17 | Baseline, Release (10-15 build), 1x / 2x / 3x, `--fps_cap=1000` | 1x 64.9 / 22.0; 2x 60.9 / 22.8; 3x 56.2 / 52.8 | 1x 80.2 / 18.7; 2x 85.5 / 16.3 | GPU 53-57% at 1x, 70-84% at 2x, 97% at 3x (nvidia-smi) | CP thread 100% busy at 1x/2x (cdb samples, 0 idle); 3x GPU-bound |
| 2026-10-17 | 0021 async submission, A/B same exe, off -> on (draws=0) | 1x 68.0 / 19.8 -> 74.6 / 19.4; 2x 68.7 / 19.6 -> 76.9 / 18.3 | 1x 105.3 / 13.5 -> 117.6 / 11.5; 2x 89.5 / 15.3 -> 116.2 / 15.6 | 2x drive now 98-99% | Keep. Pan 1x 77.2 -> 91.2, pan 2x 77.9 -> 97.9. No device-removed/TDR, identical error counts, spawn screenshots match |
| 2026-10-17 | 0021 + `d3d12_async_submission_draws` 500 / 2000 (1x) | 500: 79.2 / 17.1; 2000: 79.3 / 16.8 | (pan 500: 99.3 / 13.9; 2000: 98.5 / 14.6) | | Default 2000: same gain as 500 with ~6 vs ~16 submissions/frame |
| 2026-10-17 | 0022 lock-free shared memory check, `gpu_invalidation_expand_pages` 64 / 16 / 4, on top of 0021 draws=2000 (1x) | base 80.0 / 16.9; 64: 84.6 / 16.2; **16: 89.9 / 14.9**; 4: 84.3 / 16.4 | replay diverges after ~50 s; in the repeating 25-50 s stretch every 0022 setting is ahead | | Keep, default 16. Pan 85.5 -> 111.9 (1% low 16.2 -> 12.3). 88-94% of requests lock-free. Still ~16-25 MB/frame uploaded, mostly not CPU-write invalidation |
| 2026-10-17 | `clear_memory_page_state` false vs true (SDK default true), on 0021 + 0022 e16 | 1x 94.9 / 15.2 vs 90.2 / 15.4; 2x 91.7 / 15.4 vs 84.3 / 16.0 | (garage replay, path diverges) | | Game default false (`ApplyGameDefaults`). Uploads 19-23 -> ~2 MB/frame; pan 1x 111.4 -> 129.7, pan 2x 109.8 -> 121.6. Spawn, garage and 3 city-teleport screenshot pairs match; same warning/error types. Not covered unattended: explosions, ragdolls, car deformation |
| 2026-10-17 | 0023 (skip unchanged constant writes) off vs on; new **street** bench `--input_replay=bench/stand60.rec --teleport=1000,80,-1000` | 1x 89.1 vs 89.3; 2x 87.1 vs 87.0 | street 1x 54.2 / 28.0 vs 53.3 / 28.6 | street: GPU ~41% | Dropped 0023: neutral, and its scalar compare + byte_swap were ~20% of CP samples on the street. Street is the new worst case (~54 fps): CP ~92% busy with register writes 30%, guest blocked in a D3D wait (sub_826BA678 -> sub_826BBEE0) |
| 2026-10-17 | 0023 (`--gpu_fast_register_ranges`, copy side-effect-free Type-0 ranges at once) off vs on, 2 runs each | spawn 1x 95.0 / 15.4 vs 108.3 / 13.0; 92.9 / 15.4 vs 107.5 / 13.2 | street 1x 56.4 / 26.7 vs 63.5 / 23.6; 55.5 / 27.3 vs 61.6 / 23.9. Pan 1x 105.0 vs 141.1; 127.1 vs 148.4 (noisy) | | Kept. Screenshots (street, spawn) match off vs on; logs identical apart from addresses; all runs exit 0. Run dir bench-0023fr-run |
| 2026-10-17 | 0024 (`--gpu_draw_state_memo`: RT ownership memo, no-op transfer early-out, sampler memo) off vs on, 2 runs each | spawn 1x 101.8 / 14.1 vs 114.3 / 12.2; 98.0 / 15.1 vs 112.1 / 12.7 | street 1x 60.9 / 24.1 vs 59.8 / 26.1; 61.7 / 24.1 vs 65.4 / 22.7; street 2x 58.6 / 24.1 vs 62.1 / 22.5. Pan 1x 135.6 vs 140.2; 134.7 vs 130.5 | | Kept: spawn +12-14%, street about +3% (noise), pan neutral. Screenshots (street, spawn, title bloom, garage) match; logs identical; 22 runs exit 0. CP profile at street: RenderTargetCache::Update 16.7% -> 10%; UpdateBindings texture side (~25%) and RequestTextures (15%) are next. Run dir bench-0024-run |
| 2026-10-17 | 0026 (`--gpu_host_threads_avoid_cpu0`: CP and submission threads off logical CPUs 0/1) off vs on, 2-3 runs each | spawn 1x 113.3 / 12.3 vs 116.5 / 11.3; 113.1 / 12.7 vs 117.4 / 11.3 | street 1x 66.1 / 21.7 vs 70.5 / 17.4; 65.4 / 22.5 vs 70.2 / 17.9; 66.1 / 22.2 vs 69.7 / 18.5; street 2x 61.6 / 22.5 vs 64.3 / 20.2. Pan 1x 128.0 vs 139.4; 127.8 vs 130.8 | | Kept. Street frames over 20 ms per 30 s: ~62 -> ~2 (over 16.7 ms: ~300 -> ~20). Cause: slow frames did identical work and cycles but ran on CPU 0 (interrupts/DPCs); per-swap CPU logging confirms the CP never lands on 0/1 with it on. Screenshots match, logs identical. Run dir bench-0026-run |
