# Launcher v2 implementation and PC review

Launcher v2 is implemented on `wip/launcher-v2`, based on `main` 44c1af1.

Windows review, 2026-10-12 (Codex takeover): the x64 LLVM/MSVC UI build
succeeds and `--smoke-ui` exits 0. The first Windows staged-replacement test
failed because native `assets\\fonts\\OFL.txt` paths were checked against a
forward-slash package allowlist. Staging inventory now uses generic UTF-8
paths, also keeping rollback journal paths portable. The existing nested-asset
replacement test reproduces this failure on Windows and passes after the fix.
All six Windows CTest cases pass outside the restricted sandbox; the sandbox
blocks the original core test's temporary profile path canonicalization.
Live controller, visual, Steam and release-update acceptance remains pending.
`core.cpp`, `platform.cpp` and the original tests are unchanged. New format,
Steam and update functions live in the separate `launcher_v2` library. The
standalone UI uses the SDK's SDL3 and ImGui plus its stb_image PNG decoder.
No ReXGlue runtime is linked into the launcher.

The front end uses agency green, a dark scrim and seven tabs. HOME exposes
Play/Install, full verification, manual update checks, Settings and Quit.
Settings remain source-preserving TOML edits, saved explicitly or before Play.
DISPLAY includes the SDK 0.10.0 monitor cvar; GRAPHICS retains the FidelityFX
build gate; GAMEPLAY includes intro skipping, audio mute and close-after-Play;
CONTROLS reports the configured input backend and exposes `mnk_mode`.
The save profiles, launch root arguments, single-instance lock, import cleanup,
quick/full checks and crash-log monitoring still call the original core.

Keyboard arrows/Enter/Escape and a controller's D-pad/left stick/A/B navigate.
Q/E and LB/RB cycle tabs. Focus has a permanent green outline; HOME also has an
accent bar. B closes a combo before backing out of the containing dialog/tab.
The internal ISO/folder browser and profile-name keyboard make those actions
available on a controller; native pickers remain available as another choice.
PS3/PS4/PS5 devices select the PlayStation prompts in `ps5/`; Xbox and other
pads select `xbox_series/`. Keyboard/mouse activity selects keyboard prompts.
SDL handles connected pads and reconnection. The window defaults to 1280x720
scaled for the initial display DPI, is resizable and remembers its dimensions.
The layout and fonts scale together with available window space.

Art is decoded only after the selected game verifies. The actual Scene.xzp
buffer is additionally checked against its manifest SHA256 even after a quick
core check, before decoding the three named entries. Missing/invalid images
fall back individually. `Documents\crackdown\launcher-art\<source-sha256>\`
contains bounded decoded RGBA caches; it is outside the package allowlist.
An art error is visible under ABOUT. Nothing from the dump is committed.
The package wire format is recorded in `docs/xui-format.md`.

Barlow/OFL fonts and Xelu/CC0 prompts ship in `assets/` beside the executable.
CMake and the package script use `assets/files.txt` as an explicit allowlist.
`LICENSES/` includes their notices and the stb license, taken from stb_image.h
because the SDK supplies no separate stb LICENSE file. Other existing notices
and the public redistribution audit remain as documented in `docs/launcher.md`.
No King Kong code, strings or game art were copied.

Update checks are off until the user chooses the first-run “Enable update
checks” button (the first/default choice), or explicitly presses Check for
updates. Choosing “Keep checks off” persists that choice. Checking the ABOUT
startup toggle also grants consent and starts a check. A history dialog can
be opened without a request. The repository link is disabled until update
checks have been authorized in that run or persisted preferences. Smoke mode
never starts network operations, even if startup checks were enabled.

`release_repository` in `v2.h` is the only source-code owner/repo constant:
`Unowa001/crackdown-recomp`. It is marked TODO pending owner confirmation of
name and visibility. No token is stored or sent. Private repositories without
anonymous Releases access currently produce an HTTP error; authenticated
private-repository updates are not implemented.

Release tags must be SemVer, e.g. `v0.2.1` or `v0.3.0-beta.1`. Stable checks use
`/releases/latest` and fetch a version-sorted history (up to 20 published
releases); the optional prerelease channel uses the history endpoint. Notes
are displayed as plain text. A release needs exactly one
`CrackdownRecomp-*.zip` paired with `<that-name>.sha256`. The package script
creates the ZIP and that SHA256 companion. Uploading/publishing is separate.
Checks and downloads use WinHTTP on Windows; POSIX reports unavailable.
Requests have timeouts, response bounds and a restricted HTTPS redirect host
list. Downloads must originate in the configured repository's Releases.

“Update now” downloads and verifies the ZIP, then uses Windows PowerShell's
built-in .NET ZipArchive to extract into a new staging directory next to the
package. It validates the entire ZIP inventory before writing any entries:
one package root, safe names, no symlinks, duplicates or protected paths, and
bounded uncompressed size. PowerShell is invoked directly, without a shell,
with a launcher-generated script; errors preserve the installed package.
The portable replacement layer validates the extracted files again against
its own allowlist. Game files, saves, `crackdown.toml`, `launcher.toml` and art
caches are excluded. The supported required files are the launcher, game exe
and manifest; the runtime DLLs, licensed assets, notices and package metadata
are also allowed.

Before replacement, original files are backed up with a persistent rollback
journal in `.launcher-update-backup/`. The running launcher is renamed to
`CrackdownLauncher.old.exe`; replacements use atomic sibling-file writes.
Copy failures/cancellation restore the originals and remove newly added files.
An interrupted journal is recovered on the next start. Recovery renames a
mapped successor before restoring it, retaining an undeletable old/failed exe
until the next start. Unmarked backup directories are preserved for inspection.
A completed marker distinguishes success from interrupted replacement.
The successor waits for the package lock to be released; a failed process
start rolls the completed replacement back. The next successful start cleans
up completed backups and old/failed executables. No in-app option allows an
update while import/verification or the game process is active.

“Add Crackdown to Steam” requires Steam to be closed. It locates the current
user's Steam installation via HKCU, visits numeric local Steam user folders,
parses binary shortcuts.vdf, preserves existing fields, appends a shortcut for
this exact launcher path, and writes with the original atomic writer. Existing
shortcut files get a uniquely named backup; duplicates report already present.
Malformed or unsupported VDF fields produce an error without rewriting that
file. Shortcuts point at the package directory and launcher, so game/profile
selection stays available. Results/errors are shown on HOME. Changes are
atomic per Steam user; if a later user fails, retry completes remaining users
without duplicating the users already updated.

Server validation commands:

```sh
cmake -S crackdown/launcher -B out/launcher-core -G Ninja \
  -DLAUNCHER_BUILD_UI=OFF -DSDK_THIRDPARTY="$HOME/DecompProject/sdk/thirdparty" \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
cmake --build out/launcher-core
ctest --test-dir out/launcher-core --output-on-failure
```

All six CTest cases pass: **720 checks** (138 existing core checks and 582 new
checks). They cover the original core plus `launcher_xzp_reader`,
`launcher_vdf_round_trip`, `launcher_update_version`,
`launcher_sha256_verification` and `launcher_staged_replacement`.
They use only synthetic files/metadata, with no network. Replacement checks
cover protected files, failure rollback, restart failure, interrupted recovery,
renaming during launcher recovery, unsafe paths and unmarked/linked folders.

A Linux UI build also works with `-DSDL_UNIX_CONSOLE_BUILD=ON` on this headless
host. Run `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software
CrackdownLauncher --smoke-ui` to sweep all seven tabs and launcher dialogs.
That proves build/render initialization and catches UI assertions; visual
quality and actual controller navigation require the PC. A separate read-only probe fully verified all 197 real game files, then decoded
all three PNGs and reloaded their RGBA caches with SDL's dummy/software renderer.
The server dump has a lowercase `default.xex` symlink; the probe selected the
regular `Default.xex` spelling in its in-memory manifest without changing any
sizes/hashes or writing the dump. Derived art stayed in `/tmp/`; the probe did
not change the committed launcher or its symlink rejection. The accent's real
entry path is `xui-assets-3\Agency-logo-big.png`, so lookup accepts unique
matching filenames inside nested entries. Linked art-cache paths are ignored.

The SDL3/ImGui
revisions used on the server match the SDK v0.10.0 tag. Windows code is guarded
by `_WIN32` and reviewed by inspection; this server has no Windows compiler.

Claude Code's Windows acceptance checklist:

1. Build with SDK **0.10.0** in the x64 LLVM/MSVC environment using
   `tools\build-launcher.bat`. Run all six CTest cases and `--smoke-ui`.
   Review the WinHTTP, PowerShell extraction and mapped-image recovery paths.
2. **Owner visual review:** no-game fallback, real backdrop/logo placement,
   agency-green accent, text legibility, selected/disabled rows, all seven tabs,
   bottom status and hints, Steam card, small/large windows and 100/150/200% DPI.
   Resize, restart and move between monitors; look for clipping and distorted
   art/glyphs. Confirm this feels like the desired console-style launcher.
3. Complete a run with only a **PS4 pad**, then Xbox and keyboard separately:
   switch every tab, edit every setting/combobox/slider, use B through nested
   combos/dialogs back to HOME, browse ISO/folder locations, cancel verification,
   create/select/open profiles using the profile keyboard, inspect errors and
   history, and confirm Quit. Test unplug/replug and device prompt changes.
   Focus must remain visible, with no unreachable control.
4. Import/select the real dump, check all three art entries and cache reload,
   then test missing/corrupt entry fallbacks using a disposable synthetic
   package (never alter the player's original dump). A changed Scene.xzp must
   fail the art hash check. Repeat the existing profile/OneDrive/Unicode,
   launch/quick/full verification, import cleanup and crash cases from
   `docs/launcher.md`; confirm Default and two named saves stay separate.
5. **Owner repository decision:** confirm repository name and visibility before
   publication. Create a disposable higher-SemVer test release with the package
   ZIP and `.zip.sha256`. Confirm zero GitHub traffic before consent or manual
   Check, persist opt-out across restart, opt in, stable/prerelease checks,
   history/notes, HTTP/offline errors, cancellation and a bad digest. On a
   disposable install, update/restart; compare protected game/save/TOML bytes,
   inject an extraction/copy/access failure, interrupt replacement, and check
   rollback/recovery plus old-exe cleanup. Never use the owner's sole working
   package to test interrupted updates.
6. With Steam closed and a backup, add for all local users, open Steam, launch
   the shortcut, and confirm existing shortcuts/fields remain. Repeat to check
   duplicate reporting; test the Steam-running refusal and Unicode/space paths.
7. Assemble a clean package using the existing packaging guardrails. Confirm
   fonts, glyphs, LICENSES and ZIP checksum are included and that game files,
   art caches, configs and generated sources are absent. Complete packaged
   Play through the title screen and New Game. Publication and the existing
   redistribution/source-materials audit remain owner decisions.

Deferred optional scope: Steam grid art is omitted; the shortcut itself is
implemented, and an image-selection/cropping treatment needs owner review.
Mouse sensitivity stays a milestone-4 placeholder as the plan requests (SDK
0.10.0 has mnk sensitivity/mouse cvars, which can be considered then). Other
potential controls such as `mnk_mouse` and the configured `input_backend`
need owner selection before gaining additional UI; guest simulation/debug cvars
are intentionally not promoted into this UI. Live Windows checks above remain
pending because this server is Linux/headless and no test release is supplied.
