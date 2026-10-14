# Claude Code review handoff: beta launcher

Branch: `wip/launcher`. Base: `a31f132` (launcher and randomizer plans).
Author: Codex. Reviewer: Claude Code, pending. No merge or public upload performed.
The owner tested the local launcher on Windows and reported: "Launcher works great!"
That report does not specify which ISO/gameplay acceptance steps were exercised.

## Review scope

Review the branch diff for the standalone SDL3/ImGui launcher in `crackdown/launcher/`,
the metadata manifest and generator, build helper, beta packaging script, notices
and documentation. No runtime hooks, generated game code, assets, executables or
direct SDK edits belong to the initial launcher change. The fix round below adds
a recorded host path patch to address Unicode handling. Existing local SDK
modifications and untracked handoff/diagnostic files were left untouched and are excluded from the commit.

The owner chose reference mode: already-extracted folders are used in place,
without copying or writing into them. ISO imports use a new staging directory
and promote it to `game/` only after validation. Existing game folders are retained.

Implementation details and rationale: [launcher.md](launcher.md).
Original requirements: [plan-launcher-installer.md](plan-launcher-installer.md).

## Evidence already collected

- Windows Release launcher builds with `tools/build-launcher.bat`.
- 67 checks pass, including synthetic trimmed/full partition images, malformed
  directory trees, cancellation/failure cleanup, TOML preservation and Unicode,
  actual child-process arguments/cwd/exit status and fresh-log selection.
- `--verify-folder game tools/game-manifest.txt` verifies the real 197-file,
  6,561,712,754-byte dump and returns the supported XEX SHA256.
- SDL `--smoke-ui` exits 0.
- PowerShell packaging script parses and refuses the dirty development checkout.
- Release import inspection finds no dependency on `TracyClient.dll`.
- Owner's Windows launcher feedback is positive.

## Focus areas

1. XDVDFS bounds, cycle/overlap checks, Windows filename safety, cancellation,
   disk-space handling and cleanup ownership. Compare extraction of actual
   trimmed and full ISO images against extract-xiso, including every file hash.
2. TOML source-position edits, nested-key flattening, preservation of unknown
   settings/comments, default resets and atomic replacement. The receipt is
   informational; startup and Play recheck sizes, XEX and small-file hashes.
3. Native dialog callback/thread lifetime, startup state, early-exit log display
   and Play through title screen and New Game. Close-after-Play opts out of
   launcher monitoring. Current build exposes bilinear only because FidelityFX
   is off in the installed SDK.
4. Play passes `--game_data_root` explicitly: SDK path resolution precedes config
   loading, so relying on TOML alone breaks reference mode. The fix round also
   patches native UTF-8 path decoding in the SDK.
5. Package script's clean-tree enforcement, temporary SDK index comparison with
   the patch stack, Release build paths, dependency allowlist and metadata-only
   contents. End-to-end ZIP packaging has not been exercised on a clean checkout.
6. Complete the third-party notice/source/relinking audit before public distribution,
   especially static FFmpeg/libmspack use. The notice collector alone is not a
   completed redistribution audit. Owner confirmation of public prebuilt-executable
   distribution remains pending.

## Review workflow

On the build server, fetch origin and inspect `wip/launcher`. Use an isolated
worktree if another task is active. Build the portable core with:

```sh
cmake -S crackdown/launcher -B out/launcher-core -DLAUNCHER_BUILD_UI=OFF
cmake --build out/launcher-core
ctest --test-dir out/launcher-core --output-on-failure
```

Report findings with severity and file/line evidence. Fix findings in follow-up
commits, run the relevant checks, then return the branch for owner review. Do not
merge or publish from this handoff alone. After installer review/acceptance, scope
the randomizer starting with orb IDs, skill state and the one-orb/Progressive-Agility
vertical slice from [plan-randomizer.md](plan-randomizer.md).

## Review result (Claude Code, 2026-10-12)

Reviewed `02a567b` in full. Fixed in the follow-up commit; still not merged or published.

| # | Severity | Finding | Fix |
|---|----------|---------|-----|
| 1 | High | Files over 1 MiB (117 of 197, about 6.5 GB) were only size-checked, even right after an ISO import, so a damaged dump passed. The plan said "hash once after extraction". Reproduced: one flipped byte in `Audio/Music.xwb` passed. | The manifest has SHA256 for every file. Import and folder selection run a full check once (14 s for the real dump), "Verify all files" runs it on request, and startup/Play stay quick. `--verify-folder ... --full` for headless use. |
| 2 | Medium | A leftover `crackdown.toml.new` (launcher killed mid-write) blocked every later save and Play, because Play saves settings first (`core.cpp` `update_config`). | The stale temp file is removed before writing. |
| 3 | Medium | `fs::remove_all` in the cleanup `catch` blocks could throw (a file held by antivirus, for example) and replace the real error, such as "Cancelled" or a hash mismatch. | Cleanup uses the `error_code` overload. |
| 4 | Medium | A power loss or killed launcher during an import left a hidden 6.5 GB `game-install-*` folder that nothing reported. | Listed on the next start, with a "Delete unfinished import" button. It isn't deleted automatically, because another launcher instance could still be writing it. |
| 5 | Low | A crash after the first 10 s showed nothing in the launcher. | Any non-zero exit (a normal quit is 0) shows the code and the new log's tail. |
| 6 | Low | `package-beta.ps1` used `vswhere -latest` vcvars for the SDK build. On the owner's PC that picks Community, but the SDK PCH was built by Build Tools, so the build fails. | `-VisualStudio <path>` parameter; vswhere now also sees Build Tools (`-products *`). |
| 7 | Nit | `verify_folder` checked `xex.empty()` twice. | Removed. |

Checked without finding a problem: XDVDFS bounds, cycle, overlap and name checks; staging/rename ownership;
command-line quoting; picker callback lifetime; TOML flattening (matches the SDK's `ApplyTomlTable`);
cvar names (`resolution_scale`, `window_width/height`, `fullscreen`, `audio_mute`, `vsync`, `fps_cap`,
`present_effect`, `game_data_root` all exist); the 30 fps preset (`vsync=true`) matches `ApplyGameDefaults`.

Evidence: real full ISO import (68 s with the full check), all 197 files byte-identical to the
extract-xiso `game/`; the regenerated manifest equals the hashes of that extraction; 71 core checks
pass; `--smoke-ui` exits 0. Not exercised: a real trimmed XISO, packaged Play to New Game, and an
end-to-end package build on a clean checkout. The third-party redistribution audit (focus area 6) is still open.

## Fix round (Codex)

2026-10-13: fixes for the FIX FIRST review of `02a567b..1f55381` in
`~/reviews/2026-10-12-xui/codex_review_launcher.md`. Author: Codex;
Claude Code review and PC build/runtime tests pending. Host path/configuration
changes only; no guest addresses, instruction changes, hooks or game data.

| Finding | Fix and regression coverage |
|---|---|
| 1. Cleanup deletes unrelated/active data | Per-package startup lock; extraction immediately writes a versioned PID/start marker, removed before rename. Only valid marked dead-owner folders qualify. Canonical selected folders are excluded, and the UI rechecks before deletion. Tests cover absent/invalid/zero-PID markers, live owners, reused PIDs, exited children and selected-folder protection. |
| 2. Default saves/shared cache disagree with SDK | All profiles pass explicit absolute save/cache flags. Default is Documents/crackdown; named profiles live below profiles/; cache always remains Documents/crackdown/cache. Only the two root environment overrides are removed in the child because SDK environment application follows CLI parsing. Tests cover default/named roots and filtered child versus untouched parent environment. |
| 3. Relative save roots disagree | TOML user_data_root is ignored; native default root is made absolute. profile_args normalizes its input before forming flags. A relative-root test confirms identical absolute arguments. PC acceptance covers another launcher working directory and conflicting TOML roots. |
| 4. Windows Unicode paths | SDK patch 0009 uses existing rex::to_path for all five path cvars in SetupEnvironment. Applies atop 0001–0008 and reverse-applies independently; no lines added by another patch are edited. Unicode arguments are covered in the core/child tests; actual Windows SDK destinations require PC acceptance. |
| 5. Shared .new file races | ConfigTemp exclusively creates .new.pid.counter with O_EXCL/CREATE_NEW; collisions are skipped. Write/flush/close errors precede atomic replacement. Only its own temp is removed. Tests preserve an existing collision, two simultaneous temps and old .new files, and check failed replacement cleanup. |
| 6. Profiles follow links at launch | profile_args rechecks parent and leaf symlinks/reparse points; creation/listing also reject them. Documents itself may be redirected. Symlink tests exercise both levels and a redirected root, with graceful skip if creation is unavailable. PC acceptance adds junctions. |
| 7. Explicit Visual Studio ignored for launcher | package-beta.ps1 forwards its selected install through LAUNCHER_VS and restores the old variable. build-launcher.bat honors it, falling back to vswhere -latest -products *. Clean explicit-install and Build Tools-only packaging remain PC tests. |
| 8. Profile preference errors disappear | save_profile propagates errors and commits the in-memory selection after persistence. Create profile clears the message only after success. PC acceptance must induce a launcher.toml replacement failure and check the visible error. |
| 9. Folder URLs break #/% | Windows uses ShellExecuteW open with the native wide path for saves/logs; POSIX retains SDL_OpenURL with byte-wise percent encoding. Tests cover Unicode, space, # and % encoding. PC acceptance covers native opening. |

Server evidence: Clang 20 portable build with
`-DLAUNCHER_BUILD_UI=OFF -DSDK_THIRDPARTY=$HOME/DecompProject/sdk/thirdparty`;
`ctest -V` passes **1/1 test, 138 assertions**. Linux `main.cpp` syntax checking
passes against the same SDK dependencies. Existing SHA256, XDVDFS, cancellation,
cleanup, TOML and log checks remain passing. New verification tests supply a fixture
XEX hash while the production default remains `supported_xex`, reach successful
quick/full checks, detect small corruption in quick and large corruption only in
full, cancel mid-hash, and assert progress never decreases and ends at total.
The child receives all three arguments including Unicode, a trailing Windows
backslash and an embedded quote, and a second run exits nonzero after ten seconds.

Patch evidence: scratch SDK `/tmp/launcher-sdk-patches-eepyw_de` was reconstructed
from the pinned Git commit in `~/DecompProject/sdk`. The repository apply script
successfully applied 0001–0009 and checked every reverse application with the whole
stack present; a repeated run skipped all patches. The source SDK was not modified.

Windows-only paths remain behind `_WIN32` and have been inspected, including wide
mutex/folder/config/process APIs and filtered Unicode child environment construction.
Claude Code must build and run the Windows suite and UI, verify real save/reload
isolation across Default and two profiles with a shared cache, test non-ASCII game
and redirected Documents paths with SDK 0009 installed, conflicting TOML/environment
roots and another cwd, profile junction replacement, preference-save failures,
second-instance exclusion and interrupted-import cleanup, picker/cancel/folder
opening and early/later crash reporting. Close the remaining integration gaps with
a clean full package build using explicit `-VisualStudio`, Build Tools-only fallback,
packaged Play to New Game, and a real trimmed XISO checked against extract-xiso.
Third-party redistribution acceptance remains outside this fix round.

## Windows acceptance (Claude Code, 2026-10-12) and re-review

- Codex re-review of 1f55381..0c13754: all nine findings fixed; one Low doc error (SDK precedence is command line >
  environment > TOML, `cvar.cpp` `Outranks`), corrected here. Verdict MERGE.
- Windows Release build of 0c13754: 137 checks pass (one symlink case skipped without the privilege); `--smoke-ui`
  exits 0; a second instance shows "already running" and exits.
- SDK patch 0009 on the PC: SDK rebuilt with 0001-0009; the game booted with `--game_data_root`, `--user_data_root`
  and `--cache_root` all under a folder named `unicode-é` (game folder `gamé` a junction to the dump): intro videos
  opened from it and the shader cache and patched menu were written to `caché` (log crackdown_003 of the pc-options
  build). Before 0009 these paths were converted through the ANSI code page.
- Still open (not blocking): real trimmed XISO, a clean-checkout package build, and packaged New Game/save isolation
  across profiles by hand.
