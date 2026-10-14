# Plan: launcher v2 (console-style, King Kong inspired)

Status: implemented on `wip/launcher-v2`, 2026-10-13; Windows review/test pending.
Implementation and acceptance handoff: [launcher-v2.md](launcher-v2.md).
Original plan, 2026-10-12. Owner approved the direction. Codex implements on `wip/launcher-v2` (from `main`);
Claude Code reviews, tests on the PC and merges.

## Reference, not source

The owner's favourite launcher is TekRant Gaming's King Kong recompile (`C:\Recomps\KingKong`,
github.com/TekRantGaming/king-kong-recompiled). That repository has **no license**: use it only as a design reference
(screenshots, behaviour). Never copy its code, text or art. Its `glyphs/` folder is CC0 (Xelu's controller prompts,
Kenney input prompts); those images may be copied with their LICENSE note.

What it does well, and what we want in our own form:
- Full-window key-art backdrop extracted from the player's own game files, game logo top left.
- Big console-style text menu on the left; the selected row gets an accent bar. Controller, keyboard and mouse all work.
- Settings in tabs across the top. A status row at the bottom (game state, version). Input hints bottom right.
- Update check against GitHub Releases, "Add to Steam" card.

## Scope

Keep the standalone `CrackdownLauncher.exe` (SDL3 + Dear ImGui, no ReXGlue runtime) and every existing behaviour and
test: verified ISO import and folder selection, full/quick hash checks, unfinished-import cleanup, single instance,
save profiles with explicit roots, crash/log reporting, source-preserving TOML edits. This is a new front end over the
same core, not a rewrite of the core.

### Look
- **Art from the player's files, never shipped:** after the game files verify, read PNG entries from
  `game\XUIFrontEnd\Scene.xzp` with a small C++ XZP reader (format: docs/xui-format.md; Python reference
  tools/xui/xzp.py): `AgencyBuildingRotation.png` (backdrop), `LogoCrackdown.png` (logo), `Agency-logo-big.png`
  (accent). Decode with stb_image (sdk/thirdparty/stb). Cache decoded art under the user data folder
  (`Documents\crackdown\launcher-art\`), keyed by the source file hash. Without game files: dark gradient, text logo.
  Verify each entry exists; if one is missing, fall back gracefully.
- Fonts: bundle one OFL-licensed condensed sans for headings and one for body text (e.g. Barlow / Barlow Condensed),
  with their licenses in LICENSES/. Scale with window DPI and size; default window 1280x720, resizable, remembers size.
- Colour: dark scrim over the backdrop for legibility; one accent colour (Crackdown agency green or orange; propose).

### Layout
- Top tabs: HOME, DISPLAY, GRAPHICS, GAMEPLAY, CONTROLS, SAVES, ABOUT. LB/RB or Q/E switch tabs (show the glyphs).
- HOME menu: PLAY (or INSTALL GAME when no verified game), VERIFY GAME FILES, CHECK FOR UPDATES, SETTINGS (jumps to
  DISPLAY), QUIT. Disabled rows look disabled.
- Status row: GAME (Ready to play / Not installed / Checking...), PROFILE (name), VERSION.
- Cards bottom right: "Add Crackdown to Steam"; nothing else for now (no Discord).
- Input hints bottom right with glyphs that follow the last used device: Xbox, PlayStation (the owner uses a PS4 pad),
  keyboard. Use the CC0 glyph sets.
- Tabs:
  - DISPLAY: fullscreen/windowed, window size, monitor (if the SDK has a cvar for it; otherwise omit).
  - GRAPHICS: internal resolution (720p/1440p), upscale filter (only when FidelityFX is built in), frame rate (60/30).
  - GAMEPLAY: skip intro logos (`skip_intro_videos`), other existing game cvars that are user-facing; note candidates.
  - CONTROLS: input backend info, mouse/keyboard mode (`mnk_mode`), placeholder for mouse sensitivity (milestone 4).
  - SAVES: the existing save profiles UI (list, create, open folder) restyled.
  - ABOUT: version, licenses, repository link, open logs folder, copy system info.
- Busy states (import, verify) show a progress bar and Cancel in the HOME area.

### Input
SDL gamepad (D-pad/left stick move, A select, B back, LB/RB tabs), keyboard (arrows, Enter, Esc, Q/E), mouse (hover,
click, wheel). Focus is always visible. Every control is reachable without a mouse.

### Updates (GitHub Releases)
- Repository: `Unowa001/<name>` (owner will confirm the name and visibility before anything is published; keep the
  owner/repo in one constant). Endpoint `https://api.github.com/repos/<owner>/<repo>/releases/latest`; optional beta
  channel = latest prerelease.
- Opt-in toggle "Check for updates when the launcher opens" (default on, asked once on first start); manual button
  always works. WinHTTP on Windows (no new dependency); on POSIX the feature can be disabled.
- "Update now": download the release ZIP asset and its `.sha256` asset, verify, extract to a staging folder next to the
  package, then replace files. The running launcher renames itself to `CrackdownLauncher.old.exe` (allowed on Windows),
  copies the new files, restarts, and deletes the `.old` file on the next start. Never touch `game\`, saves, or
  `crackdown.toml`. Any failure leaves the current install intact and shows the error.
- Show the release notes (Markdown as plain text is fine) and a version history list.

### Add to Steam
Write a non-Steam shortcut into each local Steam user's `userdata\<id>\config\shortcuts.vdf` (binary VDF; read,
append, write atomically, keep a backup). Exe = CrackdownLauncher.exe, start dir = package dir. Optionally write grid
art from the extracted backdrop/logo into `config\grid\`. Steam must be closed (detect and tell the user). If the
shortcut already exists, say so.

### Not in scope
Telemetry/online player counts, Discord, music, localisation (English only for now), launching without the launcher.

## Acceptance
- All existing core tests pass; new tests for the XZP reader (synthetic package), VDF writer (round trip), update
  version comparison, SHA256 verification and staged replacement (no network in tests).
- Windows: controller-only run-through (PS4 and keyboard), art extraction from the real dump, update check against
  a test release (owner creates it), Add to Steam with Steam closed.
- Package script includes fonts, glyphs and their licenses.
