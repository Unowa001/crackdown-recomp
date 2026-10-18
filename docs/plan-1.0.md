# 1.0 feature pass (2026-10-17)

Work lands on `claude/project-thread-30a058` (GitHub main plus 1.0 features) and merges by PR. DLC
and the title update are 1.1. Build 1.0 release candidates from GitHub main, not the PC's local main
(it carries unpushed DLC commits).

| # | Feature | State |
|---|---|---|
| 1 | Launcher: 4K internal resolution, FXAA, anisotropic filtering | passed PC check (0290803) |
| 2 | Ultrawide / other aspect ratios (`--aspect_ratio`, launcher "Aspect ratio") | Austin: ultrawide looked good (PC check 2) |
| 3 | FPS counter (F3) and fullscreen/window toggle (F11) | passed PC check (0290803) |
| 4 | Achievements overlay (F7) | works with icons; text is small at 1440p |
| 5 | Frame rates above 60 (launcher 120 / 144; host frame pacing above 60) | code in this branch, needs PC check 3; ships only if every rate passes |

## Rules for PC checks

Use a separate worktree of this branch and an SDK with GitHub main's patch stack (0001-0024, 0026;
no 0025). Don't touch the shared main checkout, its `sdk/` or `patches/`, or the user's saves (replay
against a scratch profile copy). Keep local paths out of committed docs.

## PC check 1 (0290803): passed 2026-10-17

Builds and launcher tests (6/6) pass. Launcher saves and reads back `resolution_scale`,
`swap_post_effect`, `anisotropic_override`, and removes them at the defaults. In game: 4K renders
sharper at 61 fps (capped); FXAA softens edges 6-22%; 16x filtering looks like 4x at spawn (no steep
surfaces there); F3 shows "Guest: 61.6 FPS (16.24 ms)"; F11 goes fullscreen -> window -> fullscreen
while rendering. Logs normal. F7 lists achievements with icons ("3 / 43 unlocked"). Ultrawide lead
confirmed (docs/re-notes.md, 0x82BAA498).

Nits fixed after it: combo entries had a leading space; removing a setting left a blank line in
`crackdown.toml`; the RenderDoc capture key moved from F11 to F9 so F11 only toggles fullscreen.

## PC check 2: aspect ratio

1. Build Release `crackdown.exe` and `CrackdownLauncher.exe`; run the launcher tests.
2. Launcher GRAPHICS tab: "Aspect ratio" (16:9 default, 16:10, 21:9, 32:9, 4:3) saves
   `aspect_ratio = "21:9"` etc. and removes it at 16:9; the combo labels line up; removing a setting
   leaves no blank line.
3. Game, windowed, at `--aspect_ratio=21:9`, `32:9`, `4:3` and default: log shows
   `--video_mode_width=1680/2560/960 --video_mode_height=720 (aspect_ratio)`. Screenshots at spawn,
   on the title screen, the main menu, the pause menu and the HUD in the city. Check that the 3D view
   is Hor+ (same vertical view, wider sides, nothing stretched) and whether menus, HUD, the minimap
   and videos are stretched, offset or cut off. Also try `--aspect_ratio=21:9 --resolution_scale=2`.
4. F9 under RenderDoc only if RenderDoc is installed; otherwise skip.

## PC check 3: 120 and 144 fps

Austin's rule: every rate above 60 is checked for audio, stairs speed, physics and driving before it
ships. Run each item at stock 30 (`--vsync=true`), 60 (default), 120 (`--fps_cap=120`) and 144
(`--fps_cap=144`). Use the bench recordings in `crackdown/bench/` and compare 120/144 against 30 and
60, as in the 2026-10-12 60 fps checks (docs/re-notes.md "Player character", "Vehicles", "Director
VO timeout investigation").

1. Pacing: the log shows `fps_cap: guest frame cap 32 -> 1000 (host paces 120 fps)`. Spawn stand
   (`--input_replay=bench/stand60.rec`) with `--frame_log` + `tools/frametimes.py`: average near the
   cap (120.0 / 144.0, not 125 / 166), even frame times. 60 stays on the guest limiter (about 61.6).
2. Stairs: `bench/stairs_tp.rec --teleport=1396.6,19.8,-1413.1 --diag_char_proxy`, peak horizontal
   speed about 12 m/s at every rate (30 fps 11.86, 60 fps 11.96).
3. Driving: `bench/car_tp.rec --diag_vehicle`, speed at 0.5 / 1.0 / 1.5 s about 7.9 / 14.1 / 18.5 m/s
   (30 fps) at every rate, and the same wall hit. Agency stairs drive
   (`--teleport_vehicle=1396.6,19.8,-1420,0`): damage about 12%, no instant wreck.
4. Physics: `--diag_physics_dt` shows the Havok step following the frame time (about 0.008 at 120,
   0.007 at 144), not 0.016 or 0.033. Note any physics-object jitter or launched props in a city
   drive (`bench/drive60.rec`).
5. Audio: `bench/drive60.rec` completes. Director lines are not cut off (`VOA_Eng[656]` about
   5.3 s with `--diag_audio`), and music does not stop. Error set same as at 60.
6. Look for anything else that speeds up: the anim clock sub_82330888 already runs 2x at 60 (crowd /
   packed anim), and the cosmetic timer sub_823FF3B8. Note anything worse at 120 / 144.
7. Launcher: Frame rate offers 60 / 30 / 120 / 144; saving 120 writes `fps_cap = 120` and no
   `vsync`; 60 removes both.
