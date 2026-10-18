# 1.0 feature pass (2026-10-17)

Work lands on `claude/project-thread-30a058` (GitHub main plus 1.0 features) and merges by PR. DLC
and the title update are 1.1. Build 1.0 release candidates from GitHub main, not the PC's local main
(it carries unpushed DLC commits).

| # | Feature | State |
|---|---|---|
| 1 | Launcher: 4K internal resolution, FXAA, anisotropic filtering | passed PC check (0290803) |
| 2 | Ultrawide / other aspect ratios (`--aspect_ratio`, launcher "Aspect ratio") | code in this branch, needs PC check 2 |
| 3 | FPS counter (F3) and fullscreen/window toggle (F11) | passed PC check (0290803) |
| 4 | Achievements overlay (F7) | works with icons; text is small at 1440p |
| 5 | Frame rates above 60 | audit + audio/stairs/physics/driving checks at each rate |

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
