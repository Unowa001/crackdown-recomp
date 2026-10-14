# Windows launcher acceptance and Defender blocker

Continued diagnosis is recorded in [Defender investigation](launcher-defender-investigation.md),
including source/build review, local support bundle and a Microsoft review draft.

Work is on `wip/launcher-randomizer-integration`; main is unchanged. This follows
the M1 state-core commit `6b971a5`.

## Completed locally

- Combined Release game build succeeded against the existing local SDK 0.10.0
  install. Generated sources were copied into this worktree, then regenerated
  locally. `sdk` and `game` junctions point to the original workspace directories.
  Git commands inspecting the junction SDK can report a relative gitdir error;
  use `--ignore-submodules=all` for this review checkout's status. This is not a
  release-tree/SDK redistribution audit.
- Six launcher Windows CTest cases pass outside the sandbox's known temporary
  profile canonicalization restriction. The host-only M1 state tests passed in
  the preceding task. No Python recipes or game hooks changed in this polish pass.
- A development install was assembled with explicit inputs at
  `C:\Recomps\CrackdownRecomp-launcher-review`: new combined game executable,
  launcher, existing runtime DLLs, manifest, licensed fonts/prompts/notices and
  both XUI recipes. It is a local development review, not a distribution package.
- The owner's World ISO passed version/catalog validation; all 197 files were
  extracted through the launcher's production `install_image` function, fully
  verified before promotion, then independently fully verified again. A subsequent
  import refused the existing `game/` folder without replacing it.
- The installed combined game reached isolated New Game, ran its bounded replay
  and exited with code zero. User/cache roots were isolated under the workspace;
  existing owner saves were not loaded. Music was muted for automation, so no
  live audio acceptance is claimed. Existing GPU resolve warnings remain in its log.
- UI captures swept seven tabs and five dialogs at 960x600, 1280x720 and 1920x1080
  with verified game art, plus a no-game fallback at 960x600. Minimum-size settings,
  browser, profile keyboard and update dialog fit; this is size validation, not a
  physical multi-monitor/DPI/controller acceptance test.

Evidence is retained under `out\launcher-install-review` (local, not in git):
`import.log`, `full-verify.log`, `installed-new-game.log`, final size/fallback PNGs
and the isolated replay/save/cache. These game-derived captures are not committed.
Local helpers under `tools/diag/` are untracked.

## Source polish

The derived logo texture removes its opaque black matte and crops the source's
wide margins; its cache has a new suffix so older opaque caches cannot override
the change. Source game files remain byte-identical. The version aligns beside
the logo. Settings frames, sliders and modal headers use agency green; helper
text has higher contrast. Internal milestone/TODO wording is removed from UI copy.
Window close with pending settings routes to the existing Save/Quit/Back dialog;
a saved launcher retains normal one-step window close.

Two optional developer CLI paths make acceptance repeatable:

```text
CrackdownLauncher.exe --install-iso <iso> <existing-package-directory>
CrackdownLauncher.exe --review-ui <capture-directory> <width> <height>
```

The install path reads that package's manifest and uses its instance lock and
normal importer. Review mode disables update consent/network checks, waits for
startup verification, sweeps the UI and saves BMPs. Sizes must be 960-3840 by
600-2160; a capture failure returns a failure exit status.

## Blocking Defender result — do not call the final launcher ready

At 00:34:23 EDT, Windows Defender detected `Trojan:Win32/Bearfoos.A!ml`, threat
ID `2147731250`, in the new installed `CrackdownLauncher.exe`. At 00:34:48 it
detected the same threat in `out/full-integration/out/launcher-core/CrackdownLauncher.exe`.
Event 1117 confirms both were quarantined successfully. Detection source was
real-time FastPath protection during binary inspection, not a downloaded release
or ISO import. Intelligence version: `1.459.688.0`; engine: `1.1.26090.9`.

The source diff consists of the diagnostic paths and UI changes above. No new
third-party dependency was downloaded. A false positive is plausible from this
provenance, but is **not confirmed**. Do not restore/allowlist the binary or weaken
Defender to finish acceptance. Executions/rebuilds were stopped on discovery.
Defender's targeted scans of the earlier launcher-v2 binary, clang++ and lld-link
completed with no new detections. The remaining disposable copies of the new
binary were also submitted to local Defender scans for quarantine/remediation.

An attempted synthetic keyboard test in a hidden window did not establish a
pending setting before close. It is not successful keyboard acceptance or proof
of a native close bug. Final dependency inspection was interrupted by Defender.
Physical PS4/Xbox acceptance, setting edit/Save/Back/unsaved-close live acceptance,
music, Steam and live-release updater tests remain pending.

Next: retain quarantine and ask Microsoft to classify the detection through its
[file-analysis process](https://www.microsoft.com/en-us/wdsi/filesubmission).
No samples were uploaded, no exclusions created, and no quarantine restored in
this session. Do not rebuild/redeploy this final launcher until the alert has
been investigated. The full verified ISO extraction and successful isolated game
run are preserved independently of the launcher blocker.
