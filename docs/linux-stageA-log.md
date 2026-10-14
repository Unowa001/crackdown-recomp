# Linux Stage A log (2026-10-13)

Plan: `docs/plan-linux.md`. Host: the LAN build server (Ubuntu, 16 cores, 30 GB, clang-20,
**NVIDIA GeForce RTX 2080**, driver 610.57.04, Vulkan 1.4). No monitor: runs use Xvfb.

## Done

- Worktree `~/worktrees/linux-m2` (detached `wip/m2` 639f203), SDK f5337cd (v0.10) with patches
  0001-0009: **all nine apply cleanly** and pass the reverse-apply check (`tools/linux/linux_stageA_sdk.sh`).
- Packages the SDK/SDL needs that were missing on the server (now installed):
  `libwayland-dev` (configure fails without it) and, for SDL3's video/audio backends,
  `libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev
  libxkbcommon-dev libwayland-bin wayland-protocols libdecor-0-dev libegl-dev libdrm-dev libgbm-dev
  libpulse-dev libpipewire-0.3-dev libasound2-dev libudev-dev libdbus-1-dev`.
  **Trap:** without them SDL3 builds silently with only the offscreen video driver and no audio
  driver; the game then fails at start with `SDL_InitSubSystem(SDL_INIT_VIDEO) failed: x11 not
  available` even on a real display. Check `SDL_build_config.h` for `SDL_VIDEO_DRIVER_X11`.
  Test tools: `xvfb mesa-utils vulkan-tools imagemagick gdb`.
- `REXGLUE_BUILD_TESTS=ON` fails: `tests/unit/core/hash_test.cpp` can't find `xxhash.h` (SDK test
  target include path). SDK builds with tests OFF.
- Codegen + `linux-amd64-release` game build: links (`crackdown` + `librexgpu-xenos.so`); AP host
  tests 9/9 pass (`tools/linux/linux_stageA_game.sh`).
- Boot smoke (`tools/linux/linux_smoke.sh`, Xvfb + chosen ICD, isolated user data, screenshots):
  - lavapipe: title screen at 60 s, attract trailer at 180 s (`out/linux-smoke/lvp/` on the PC).
  - **NVIDIA RTX 2080: title screen at 60 s** (`out/linux-smoke/nvidia/`). The server is a usable
    real-GPU Linux test host for unattended runs.
  - With a copied save profile (`save1-user-data`) and `stairs_tp.rec`: main menu + city flyover
    render, including the PC "Quit to Desktop" entry (`out/linux-smoke/stairs-nv/grid.png`).

## Open defects

1. **FIXED by `patches/sdk/0010-xam-XamEnumerate-reports-zero-items-on-every-error.patch`.**
   Root cause (read-only Codex analysis, `out/reviews/2026-10-13/codex_linux_av.md`): the thread is
   the Massive ad-server lookup ("X360 LSP"); it ignores the error from `XamEnumerate` on the
   never-valid handle of `XTitleServerCreateEnumerator`, and `XamEnumerate_entry` copied an
   uninitialized local into `items_returned` on that path. A garbage count walks 208-byte records past
   the 10-record stack buffer; record 11 = guest 0x706A0008. On Windows the stale value is plausibly zero (not measured; no AV there either way).
   After the patch: same replay, **0 AV lines** (was thousands), flyover cycles normally
   (`out/linux-smoke/stairs-p10/grid.png`). The POSIX exception handler still resumes an unhandled
   fault in a loop (Windows continues the search): separate SDK issue, not fixed yet.
   Original report: **Guest access-violation flood on the main menu (Linux only).** ~5 s after the main menu, right
   after `agencybuildingrotation.bik` opens, a guest thread faults repeatedly reading guest
   `0x706A0008` (8 bytes into the guard page above its stack top; `xthread.cpp` AllocateStack).
   Backtrace (`tools/linux/gdb_av.sh`): thread entry `sub_82747280` -> `sub_82AA0920` ->
   `sub_82AB93C0` (`lbz r11,0(r3)`, r3 = r1+2232). 0 of 119 Windows logs show it. Replay stalls on
   the main menu. Analysis delegated (read-only Codex, `out/reviews/2026-10-13/codex_linux_av.md`).
2. The game ignores SIGINT; the smoke script must SIGKILL it (exit 137). Needs a clean-exit path.
3. Replay `stairs_tp.rec` reaches the main menu but never selects Campaign, also with a warm shader
   cache (`CACHE=` in linux_smoke.sh; `out/linux-smoke/stairs-p10b/grid.png`). Likely cause: no
   gamepad on the server, so XInputGetState reports "not connected" and the replayed pad presses are
   ignored (title passed via replayed keystrokes). Next: make `hooks_input_replay.cpp` report pad 0
   connected while replaying (or a virtual uinput pad), then rerun; unblocks unattended Linux gameplay.
4. Later flyover frames look posterized (banded sky): likely the readback_resolve / sunlight path
   (patch 0008 and readback handling are D3D12-only). Stage B.
5. `scene.xzp` lookups under `game:\xuifrontend\en-US\` and `cache:` fail (also probing on Windows?
   not yet compared).
