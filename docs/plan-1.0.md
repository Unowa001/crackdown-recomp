# 1.0 feature pass (2026-10-17)

Work lands on `claude/project-thread-30a058` (GitHub main plus 1.0 features) and merges by PR. DLC
and the title update are 1.1. Build 1.0 release candidates from GitHub main, not the PC's local main
(it carries unpushed DLC commits).

| # | Feature | State |
|---|---|---|
| 1 | Launcher: 4K internal resolution, FXAA, anisotropic filtering | code in 0290803, needs PC check |
| 2 | Ultrawide / other aspect ratios | RE needed (lead below) |
| 3 | FPS counter (F3) and fullscreen/window toggle (F11) | code in 0290803, needs PC check |
| 4 | Achievements overlay (F7) and icons | check on PC |
| 5 | Frame rates above 60 | audit + audio/stairs/physics/driving checks at each rate |

## PC check for 0290803

Use a separate worktree (for example `C:\Users\unowi\DecompProject\wt-1.0`) and an SDK with
GitHub main's patch stack (0001-0024, 0026; no 0025). Don't touch the shared main checkout, `sdk/`
or `patches/`.

1. Build Release `crackdown.exe` and `CrackdownLauncher.exe`; run the launcher tests if present.
2. Launcher GRAPHICS tab: "Internal resolution" (720p / 1440p / 4K), "Anti-aliasing" (Off / FXAA /
   FXAA extreme), "Texture filtering" (Game's own / 2x / 4x default / 8x / 16x) and the F3/F11 hint.
   Saving writes `resolution_scale=3`, `swap_post_effect="fxaa"`/`"fxaa_extreme"`,
   `anisotropic_override=-1/2/4/5` to `crackdown.toml`; choosing the defaults removes the keys.
3. Game with `--resolution_scale=3`, then `--swap_post_effect=fxaa`, then `--anisotropic_override=5`:
   a spawn screenshot each, log checked for errors.
4. F3 shows "Guest: NN FPS" (screenshot). F11 switches fullscreen and windowed both ways in game
   with no crash or black screen.
5. Ultrawide lead from saulob's README (verify, don't trust; never copy their code): camera
   constructor `sub_822846A0`, aspect float `0x82BAA498`. Find what reads/writes `0x82BAA498`, its
   default value, and the function that builds the projection from it, with addresses and
   disassembly excerpts. No patch yet.
6. Achievements: press F7 in game; screenshot the list and note whether icons show.
