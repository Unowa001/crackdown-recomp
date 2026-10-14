# Beta launcher implementation

Implemented on `wip/launcher`. The owner selected **use extracted folders in place**.
Preparing a local package is authorized; public distribution of the prebuilt game
executable remains an owner decision before upload.

Windows: run `tools\build-launcher.bat`. The standalone executable is
`crackdown/launcher/out/CrackdownLauncher.exe`; no ReXGlue runtime is linked into it.
The script builds Release, runs the core tests and places the manifest beside it.
An assembled local review folder is available at `out/launcher-preview/`; run
its launcher and select the existing `game/` folder to test reference mode.
This folder is a development preview, not a distribution ZIP.
To test Play, put the launcher and manifest beside the Release game executable
and runtime DLLs, or use the package script after committing the reviewed changes.

Portable core and tests (requires the initialized SDK dependency checkout):

```sh
cmake -S crackdown/launcher -B out/launcher-core -DLAUNCHER_BUILD_UI=OFF \
  -DSDK_THIRDPARTY="$HOME/DecompProject/sdk/thirdparty"
cmake --build out/launcher-core
ctest --test-dir out/launcher-core --output-on-failure
```

Leave `LAUNCHER_BUILD_UI=ON` to build SDL3 and ImGui. `LAUNCHER_FIDELITYFX=ON`
exposes CAS and FSR only for a matching game/SDK build. The current Windows SDK
has FidelityFX disabled, so the default UI offers bilinear filtering.

Generate the metadata manifest with `python tools/generate-game-manifest.py game`.
It checks the supported XEX and 197-file count before writing. The checked-in
manifest contains paths, sizes and the SHA256 of every file, no data.

The reader supports partition offsets 0, 0xFD90000 and 0x18300000; an XGD3
partition at 0x2080000 gets a specific error. Both descriptor signatures, directory
trees, sector ranges, name safety and case-insensitive path uniqueness are checked.
Imports validate the XEX before extraction, validate the manifest and disk space,
stream into a new temporary directory and rename it to `game` only after verification.
Existing `game` directories are never overwritten or deleted. Cancellation and
errors remove only the directory created by that import. An extracted folder is
read in place and never gets a receipt or other writes.

Every file is hashed once: an ISO import hashes the staging copy before renaming it
to `game`, and selecting an extracted folder hashes it in place (about 14 s for the
6.56 GB dump on an NVMe drive; a progress bar and Cancel are shown). "Verify all files"
repeats the full check on request. Startup and Play run the quick check: all file sizes,
the XEX and every file up to 1 MiB. The `.verified` receipt in imported folders records
the manifest/XEX hashes; it is informational, and is not trusted to bypass checks.
The launcher holds a per-package lock until exit: a named mutex derived from the
canonical package path's SHA256 on Windows (`Local\CrackdownLauncher-<hash>`),
and `flock` on `<package>/.launcher.lock` on POSIX. A second instance displays
"The launcher is already running" and exits. Different packages can run independently.

A staging folder gets `.crackdown-import` immediately after creation. Its versioned
marker records the creator PID and process start identity (Windows creation FILETIME;
Linux `/proc/<pid>/stat` start ticks). The marker is removed just before promotion to
`game/`. Cleanup only offers `game-install-*` directories with a valid marker whose
owner is dead, excluding the canonical selected game folder. Inaccessible owners
are treated as alive; other POSIX hosts conservatively check PID liveness. The UI
rechecks candidates at deletion time. Unmarked folders, including imports interrupted
before the marker was written or after its removal, need manual inspection and are
never offered for deletion. Cleanup remains an explicit button action.

Settings preserve untouched TOML text, comments, unknown values and nested tables.
Only changed controls are written. Restoring a default removes its override.
The SDK flattens nested tables with underscores; category names do not define
config sections (`sdk/src/core/cvar.cpp`, `ApplyTomlTable`). Invalid or ambiguous
configurations produce an error and are retained. Writes exclusively create a sibling
`<config>.new.<pid>.<counter>` with `CREATE_NEW` / `O_EXCL`, skipping collisions.
Write, flush and close must succeed before atomic replacement (`MoveFileExW` /
rename). Cleanup removes only that writer's temporary file; older `.new` files are
retained. Profile selection is committed in memory only after its preference save
succeeds; creation leaves any persistence error visible. Launcher preferences use a
separate `launcher.toml`.

Deviation from the initial no-arguments plan: Play passes `--game_data_root=<path>`.
`ReXApp::SetupEnvironment` resolves paths *before* loading TOML, and its later
runtime construction uses those earlier paths. Passing the selected path guarantees
reference-folder support. SDK patch `0009-utf8-path-cvars.patch` uses `rex::to_path`
to explicitly decode UTF-8 for game, user, update, cache and metadata root cvars
before constructing their native paths. This supports non-ASCII Documents and game
folders on Windows. It changes host path handling and touches no guest instructions.
The selected game path is also recorded in `crackdown.toml`. The child works in the package directory and is spawned
without a shell. Early exits within ten seconds display the tail of a newly written
game log, and so does any later non-zero exit (a crash; a normal quit exits 0).
Close-after-Play skips this monitoring by explicit user choice.

Save profiles: the launcher owns the effective save and cache roots. Play **always**
passes absolute `--user_data_root` and `--cache_root`, including Default. Default is
`<Documents>\crackdown`; a named profile is
`<Documents>\crackdown\profiles\<name>`; the cache is always
`<Documents>\crackdown\cache`. Documents is the native Windows known folder,
so redirection such as OneDrive is supported. POSIX uses the platform user-data
folder. The launcher ignores TOML `user_data_root`: the SDK resolves runtime paths
before loading that file. It preserves the existing TOML text, but that setting has
no effect on launched save destinations. Default and named profiles share the cache.
A new profile starts with no save; choose New Game. The selection lives in
`launcher.toml`, separately from game settings. Before launching or opening a named
profile, both `profiles/` and its leaf are checked again for symlinks/reparse points;
the Documents root itself may be redirected.

Actual pinned SDK precedence (`sdk/src/core/cvar.cpp` and
`src/ui/windowed_app_main_sdl.cpp`): every value records its source and a later
source only replaces it if it ranks at least as high (`Outranks`, `cvar.h` `Source`):
command line > `REX_*` environment > TOML > default. The launcher's explicit root
arguments therefore already win over the environment. Runtime roots have already been
captured when TOML loads. As a belt-and-braces measure the launcher also removes **only** `REX_USER_DATA_ROOT` and `REX_CACHE_ROOT` from the child's
inherited environment. These overrides are therefore superseded when using the
launcher. The parent's environment and other `REX_*` settings are preserved.

"Open saves folder" and "Open logs folder" use `ShellExecuteW` with the native wide
path on Windows. POSIX uses SDL_OpenURL with an absolute file URL whose UTF-8 bytes
are percent-encoded, including spaces, `#` and `%`.

Headless diagnostics:

```text
CrackdownLauncher.exe --verify-folder <folder> <manifest> [--full]
CrackdownLauncher.exe --inspect-iso <iso> <manifest>
CrackdownLauncher.exe --smoke-ui
```

Packaging: `powershell -NoProfile -ExecutionPolicy Bypass -File tools/package-beta.ps1 -Version 1`.
`-SkipBuild` packages already-reviewed Release builds. `-VisualStudio <install path>`
picks the vcvars for the SDK, game and launcher builds (forwarded as `LAUNCHER_VS`);
it must be the MSVC version that built the SDK's
precompiled header (on the owner's PC that is Build Tools, not Community).
The launcher helper honors `LAUNCHER_VS`; otherwise it uses `vswhere -latest -products *`.
The package script refuses a dirty main
repository, a mismatched SDK/dependency checkout, SDK tracked edits outside the
recorded patch stack, existing output versions and unpackaged binary dependencies.
The fixed input allowlist excludes game folders, generated sources, ISO/XEX files
and developer configs. It records revisions and patch hashes and inspects imports;
Tracy is included only if actually imported. The current Release binaries do not
import Tracy. The Microsoft Visual C++ x64 Redistributable is a prerequisite.

The SDK must have the reviewed patch stack applied before packaging; the script
checks it using a temporary Git index and does not edit the SDK or its real index.
Unrelated local SDK changes and untracked handoff/diagnostic files currently prevent
a release package, as intended. Do not discard those files to satisfy the check.

Before public release, review the collected third-party notices and redistribution
materials. The SDK statically uses LGPL components (FFmpeg and libmspack); copying
license files alone does not complete the source/relinking-materials audit. The
packaging script collects notices but does not claim that audit is complete.
The SHA256 copyright notice is preserved; the independent XDVDFS reader credits
extract-xiso as its format reference.

Real ISO, 2026-10-12 (Claude Code review): the full World ISO (7,835,492,352 bytes)
imports in 68 s including the full hash check, and all 197 files are byte-identical to
the extract-xiso dump (only `Default.xex` vs `default.xex` capitalization differs; paths
are case-insensitive). Flipping one byte in `Audio/Music.xwb` passes the quick check and
fails the full one. Remaining acceptance: a real trimmed XISO (synthetic tests cover it),
native picker/cancel/settings by hand, and packaged Play through title screen and New Game.

Validation on Windows, 2026-10-12: Release build succeeds; 67 checks pass,
including full/trimmed synthetic partition detection, malformed trees, extraction
cleanup, TOML/comment/Unicode handling, actual child-process argument quoting and
working directory, early-exit status and fresh-log selection. Headless verification
of the real 197-file extracted dump returns the supported XEX hash. The SDL GUI
smoke test exits with code 0. The package script parses and its dirty-repository
refusal was exercised. A complete release ZIP has not been produced from this
uncommitted checkout.

Fix-round server validation, 2026-10-13 (Codex): 138 assertions pass in the portable
core's single CTest case, built with Clang 20 and UI disabled. Added successful
quick/full verification, small/large corruption checks, cancellation during hashing,
monotonic progress reaching its total, marked/live/dead/selected import handling,
package lock exclusion/release, exclusive temporary file collision and replacement
failure handling, linked profile rejection and redirected-root acceptance, Unicode
arguments, three-argument child quoting (trailing backslash and embedded quote),
child environment filtering and a nonzero exit after ten seconds. Link checks skip
with a message where the OS denies symlink creation. The Linux UI translation unit
also passes syntax checking. Windows-specific paths were inspected; a Windows build
and runtime results remain pending.

The SDK patch stack was checked in `/tmp/launcher-sdk-patches-eepyw_de`, populated
from the pinned commit in `~/DecompProject/sdk` without editing that checkout.
`tools/apply-sdk-patches.sh` applied 0001–0009, all patches reverse-applied individually
with the whole stack present, and a second script run skipped every patch. Patch 0009
changes only original SDK lines, independently of 0001–0008.

Claude Code's PC acceptance must cover the Windows UI/core build and CTest, duplicate
instance dialog and import cleanup after termination, native picker/cancel/settings,
profile preference-save failures, junction rejection at both profile levels, and
opening saves/logs with spaces, `#`, `%` and Unicode. Rebuild/install the SDK with
0009 before testing non-ASCII game and Documents paths. Test Default plus two named
profiles through New Game/save/reload, switching without crossing saves and reusing
the shared cache. Repeat with conflicting absolute/relative TOML save roots,
`REX_USER_DATA_ROOT` / `REX_CACHE_ROOT`, OneDrive Documents, and launching from another
working directory. Verify early and later crash messages/logs. Complete a clean ZIP
package build with explicit `-VisualStudio` and with only Build Tools installed,
packaged Play through New Game, and a real trimmed-XISO import compared by hash
with extract-xiso. These runtime/package tests cannot be completed on this headless
Linux host; the public redistribution audit remains separate.

Launcher v2 replaces the beta front end while keeping this core behavior.
See [launcher-v2.md](launcher-v2.md) for its implementation, update/package
contract, server checks and Claude Code's Windows acceptance checklist.
