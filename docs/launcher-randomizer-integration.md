# Launcher and randomizer integration takeover

Latest Windows follow-up: [acceptance and Defender blocker](launcher-windows-acceptance-2026-10-13.md).
The combined game now builds and has passed an isolated installed New Game run;
the full 197-file ISO install passed. Final launcher polish passed visual sweeps
and six host tests, but Defender quarantined the new launcher binary. Release/
launcher readiness is blocked on investigating that detection. Older paragraphs
below describe the earlier takeover state.

2026-10-12, Codex primary takeover. Branch `wip/launcher-randomizer-integration`
combines main's AP contract probe, native PC Settings plus its verified schema
fix, and launcher v2 with the Windows path-separator fix. Main is unchanged.

Merge resolutions preserve the launcher's reviewed process/platform behavior
and move its exclusive ConfigTemp writer into the shared TOML editor. Both the
game and launcher now use that writer, including binary I/O checks and CRLF
preservation. The earlier PC Settings branch's fixed `.new` temporary file is
superseded; launcher compatibility aliases preserve the existing API/tests.

The updater and beta package now include exactly `xui/Scene.xpatch` and
`xui/menuHUD_3.xpatch`. Both are required in a staged update. The ZIP extractor
and portable inventory validator agree on those names; game-derived `.xzp`
files and arbitrary patch names remain excluded. Integration tests cover an
incomplete pair, untouched installation on refusal, and installation of both.

Validation: combined launcher Windows x64 build succeeds; all six CTest cases
pass outside the sandbox (temporary-profile canonicalization is blocked inside
it); integrated `--smoke-ui` exits 0. All 30 Python XUI tests pass. Original
launcher v2 independently passed its UI build, smoke test and six cases too.
The combined game executable has not been built/tested yet; the PC Settings
shortcut uses the existing separate worktree's tested game build.

PC Settings run 013 and the owner's 22:45:14 screenshot confirm opening,
resolution editing/persistence and Back. A subsequent recipe switches heading
and captions to existing plain XuiLabel visuals to eliminate cloned decorative
stripes. That cleanup is deployed to the shortcut, with temporary loader hooks
removed. Owner visual retest and explicit music confirmation are pending.

The full Archipelago randomizer is not implemented by this merge. The in-process
probe still logs observations only. Next work is the connected orb/skill slice
in randomizer-scope.md: pinned WebSocket/AP dependencies, validated slot data,
seed/team/slot/save binding, durable inventory/check reconciliation, game-thread
delivery, and a small developer world. The AP launcher tab follows a working
bridge; do not offer a connection that the executable cannot make. Boss gating,
weapon grant paths and broader skill persistence remain separate acceptance
work. No APWorldMaker files were edited in this takeover.

Dependency research: [apclientpp](https://github.com/black-sliver/apclientpp)
uses [MIT](https://github.com/black-sliver/apclientpp/blob/main/LICENSE), with
WebSocket/asio, JSON and optional schema dependencies. TLS requires OpenSSL and
certificate handling; compression requires zlib. Its upstream instructions and
the [AP protocol](https://github.com/ArchipelagoMW/Archipelago/blob/main/docs/network%20protocol.md)
were checked, but dependencies have not been pinned or integrated. The protocol
requires index-zero inventory replacement and resync after a receipt-index gap;
the bound-save design must reconcile permanent grants after older save loads.

Nothing was published, pushed or merged into main. Repository name/visibility
and live release-update/Steam/controller acceptance are still owner decisions.

Follow-up: the host-only M1 session state core is now implemented and tested.
See [implementation handoff](randomizer-m1-implementation.md) for its API,
five passing Windows test cases, and the ordered runtime/client work remaining.
It binds durable state, replaces/reconciles received inventory, persists offline
checks and computes absolute Agility targets after older native saves. It has no
runtime consumer yet; the live client and combined game build remain unfinished.
