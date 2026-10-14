# Plan: launcher, ISO extractor and beta package (Windows)

Status: implemented on `wip/launcher`, 2026-10-12; Windows build and core tests passed,
real ISO and packaged gameplay acceptance pending. See [implementation notes](launcher.md).
Owner selected **use extracted folders in place**. Public prebuilt-executable distribution
remains pending confirmation before upload. Other owner decisions are marked **[decide]**. Implementation can go to Codex on a `wip/launcher`
branch (worktree); Claude Code reviews, tests on the PC and merges (same model as APWorldMaker's CLAUDE.md).

## 1. Goal and non-goals

Beta testers download a zip, run `CrackdownLauncher.exe`, point it at **their own** Crackdown ISO once, pick a few
settings and press Play. No game data and no codegen on their side.

Not in scope now: in-game options overlay (roadmap step 3), auto-update, Linux packaging, the from-source build fix
(`crackdown/generated/rexglue.cmake:57,79` skips the recomp library without `generated/sources.cmake`; it only matters for
a source release).

**[decide] Distribution form.** The beta ships the prebuilt `crackdown.exe`, which contains recompiled game code but no
assets. Other static recomps ship this way (the user supplies the disc). It stays out of git (CLAUDE.md hard rule).
Confirm before the first public upload.

## 2. What ships (beta zip layout)

```
CrackdownRecomp-beta<N>/
  CrackdownLauncher.exe       new
  crackdown.exe               Release build
  rexruntime.dll, rexgpu-xenos.dll   from sdk/out/install/win-amd64/bin (Release, patches 0001-0008)
  LICENSES/                   ReXGlue SDK, Xenia (BSD), SDL3 (zlib), Dear ImGui (MIT), extract-xiso notice if any of
                              its code or offsets are used, others found by scanning the SDK's third_party list
  README.txt                  requirements (x64 CPU with AVX2/FMA3, which patch 0006 needs; D3D12 GPU), how to report bugs
  game/                       created by the launcher, never shipped
  crackdown.toml              written by the launcher
```

Check whether the Release exe needs `TracyClient.dll` (it sits in the build dir). It shouldn't: Release compiles
profiling out. Verify with `dumpbin /dependents` or llvm-objdump and leave it out if unused.

Writable paths: the SDK reads `crackdown.toml` next to the exe (`sdk/src/ui/rex_app.cpp:146`) and puts user data and
cache under `GetUserFolder()/crackdown`. The zip must therefore be extracted to a user-writable folder (not Program
Files). The README says so, and the launcher refuses with a clear message if it can't write next to itself.

## 3. Launcher

Single window, small. Use **SDL3 + Dear ImGui** (both already in the SDK tree) so it builds on the Linux server too
(useful for Codex to compile and unit-test it) and shares the look of the later in-game overlay. Native file picker via
`SDL_ShowOpenFileDialog` / `SDL_ShowOpenFolderDialog`.

New CMake target `crackdown/launcher/` (own executable, links SDL3 + imgui + a small core library; it does **not**
link rexruntime, so it starts instantly and has no game code).

### 3.1 First run: game files

States: `no game` → `verifying` → `extracting (progress)` → `verified` / `error (message)`.

Inputs accepted:
1. **ISO image** (the main path). Must handle both a full Redump-style XGD2 image (game partition at offset
   `0xFD90000`) and a trimmed XISO (partition at 0). Detect by looking for the XDVDFS magic `MICROSOFT*XBOX*MEDIA` at
   partition + `0x10000` (sector 32) for each candidate offset (0, `0xFD90000`, `0x2080000` for XGD3 just to reject it
   with a clear message, `0x18300000`).
2. **An already-extracted folder** (contains `default.xex`): copy or use it in place **[decide: copy vs reference;
   recommend reference with a stored path to save 7 GB]**.

Extraction: write our own XDVDFS reader (~300 lines, C++): volume descriptor at sector 32 (root dir sector + size),
directory entries form a binary tree (left/right subtree offsets in dwords, attributes, name length, name), files are
contiguous sector runs. Iterative traversal with an explicit stack, reject cycles/out-of-range sectors, sanitize names
(no `..`, no absolute paths, no reserved Windows names), 64-bit offsets, streaming copy with a 4 MB buffer and progress
by bytes. extract-xiso (`~/tools/extract-xiso` on the server, BSD-style licence with notice) is a **reference for the
format**; if any code is adapted, keep its copyright notice in LICENSES. Do not shell out to it.

Verification after extraction (or of a picked folder):
- `default.xex` sha256 must be one of the known dumps. Today only one is known:
  `7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a` (World multi-language, 197 files). Other XEX
  versions run a different binary than our recompiled code, so they are a hard error ("unsupported version"), with the
  hash shown so testers can report it.
- File manifest: `tools/game-manifest.txt` (path, size, optionally sha256 for the small files) generated once from
  the reference dump by a script; the launcher checks presence and size for all 197 files. Hashing 7 GB on every
  start is too slow: hash once after extraction and write `game/.verified` (manifest hash + xex hash); later starts
  check sizes only.
- The manifest holds file names and sizes only (no data), which is fine for git.

Disk space check before extracting (free space ≥ file total + 5%). Cancel button; a cancelled or failed extraction
deletes the partial `game/`.

### 3.2 Settings (written to `crackdown.toml`)

Only cvars that exist today (names checked in the SDK):

| UI | cvar | values |
|---|---|---|
| Resolution | `resolution_scale` (or `draw_resolution_scale_x/_y`) | 1 = 720p internal (default), 2 = 1440p internal (**beta: label "experimental"**; 60 fps at 2x isn't verified yet) |
| Upscale filter | `present_effect` | bilinear / cas / fsr (fsr2/fsr3 hidden until tested) |
| Fullscreen | `fullscreen` | bool |
| Window size | `window_width`, `window_height` | presets |
| Frame rate | `vsync` + `fps_cap` | "60 fps (default)" = leave both unset (ApplyGameDefaults); "30 fps (stock, quieter fans)" = `vsync = true` |
| Audio | `audio_mute` | bool |

Rules: write only keys the user changed from default, and leave unknown keys in an existing toml untouched (parse,
edit, write back; toml++ is in the SDK). Check the exact toml layout the SDK expects (section names per cvar
category, `sdk/src/core/cvar*`) and add a round-trip test.

### 3.3 Play

Launch `crackdown.exe` with the working dir = its folder, no extra arguments (paths come from the toml and the
auto-detected `game/` next to the exe; `crackdown/src/host_tuning.cpp:150-161`). Launcher stays open or closes
(setting). If the process exits within 10 s, show the tail of the newest `logs/crackdown_NNN.log`.

### 3.4 Bug-report helper

Button "Open logs folder" and "Copy system info" (GPU name/driver, CPU, Windows build, game hash, launcher version,
toml contents) for beta reports.

## 4. Packaging script

`tools/package-beta.ps1 -Version N` (PC): builds the Release SDK (BuildTools vcvars, see memory note on the PCH) and
the game + launcher, copies the files from section 2, writes `VERSION.txt` (git describe of the main repo, SDK tag and
the list of applied patches), refuses if `git status` is dirty or if any file under `game/`, `generated/` or a `.xex`
would be included, then zips. Optional later: an Inno Setup wrapper (installs per-user to
`%LOCALAPPDATA%\Programs\CrackdownRecomp`, which is writable).

## 5. Tests / acceptance

Unit (Linux server and PC, no game data in the repo):
- XDVDFS reader against a synthetic image built by the test itself (nested dirs, tree with left/right subtrees, a
  zero-length file, a name needing sanitizing, a malicious entry pointing outside the image or looping).
- toml round trip keeps unknown keys and comments where possible.

Integration (needs the real ISO; server has it for extract-xiso; PC for the full run):
- Extract the reference ISO with ours and with extract-xiso: identical file list and sha256 of every file.
- Trimmed XISO and full image both work; an XGD3 or random file gives a clear error.
- Fresh folder: unzip → launcher → pick ISO → verify → Play reaches the title screen and New Game (Claude runs this
  on the PC with `--input_replay=bench/stand60.rec` passed through a debug-only launcher option, or by hand).
- Unsupported XEX hash shows the error and the hash.

## 6. Work split and order

1. Codex (`wip/launcher` worktree on the server): XDVDFS reader + unit tests, manifest script, launcher skeleton
   (states, file dialog, extraction thread, progress), toml writer. Builds on Linux.
2. Claude Code: review, Windows build integration, PC integration test against the ISO, packaging script, LICENSES
   audit, merge.
3. Codex read-only review of the merged diff; then beta1 zip.

Estimated size: launcher + extractor ~1,200 lines C++, tests ~400, script ~150.
