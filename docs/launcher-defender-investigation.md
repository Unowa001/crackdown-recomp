# Launcher Defender investigation, 2026-10-13

Follow-up to [Windows acceptance](launcher-windows-acceptance-2026-10-13.md).
The user authorized continued diagnosis. No quarantined executable was restored
or executed, and no security setting/exclusion was changed.

## Findings

1. Review of the launcher source, compiler/linker commands and CMake post-build
   batch found no unexpected download, payload injection or executable rewrite
   in the reviewed paths. Post-build work copies fonts/prompts/notices. The scoped
   SDK dependency diff is clean, including SDL3, ImGui, crypto, stb and toml++.
2. Updater HTTPS downloading, generated PowerShell ZIP extraction, package
   replacement and restart behavior also exists in the earlier launcher. Those
   functions are reached through opt-in/manual UI actions; no updater action ran
   during the visual sweeps. These are possible heuristic features, but there is
   no evidence identifying any one of them as the detection cause.
3. Clang 23.1.3 and lld-link and the earlier launcher completed targeted Defender
   scans without new detections. The application diff from the earlier Windows
   branch contains the shared TOML-writer integration, the paired-menu updater
   inventory change and the recorded UI/diagnostic additions. No fresh dependency
   download occurred in this work.
4. The administrator Defender collector successfully produced a local support
   CAB and quarantine listing. Four copies of the new launcher were explicitly
   listed as quarantined; the remaining disposable copy also rejects file reads.
   Do not repeatedly read/recreate these files: access retriggers protection.
5. Engine logs record cloud-issued dynamic signatures at 04:34:12–13 UTC and the
   first file block at 04:34:23.949 UTC. This chronology does not prove the causal
   signature or the source feature responsible. No discovered source compromise
   explains the alert; a false positive remains plausible and unconfirmed.
6. Defender records SigSeq `0000266721e46593` and signature SHA
   `c4d2563f71fce2898e0c150c83440ec66bd742ca`. That hash belongs to its detection
   signature, **not the launcher executable**. The executable SHA256 is unavailable
   because Defender blocks access. Its metadata database is an opaque diagnostic
   format, not SQLite; no decryption or quarantine extraction was attempted.

## Local evidence, not committed or uploaded

`out\defender-launcher-investigation\` (local, not in git) contains:

- `source-provenance.json`: application commit/source hashes and the *earlier,
  unflagged* launcher hash, clearly separate from the unavailable flagged hash.
- `quarantine-list.txt`, `detections.json`, `threats.json`, admin collector transcript.
- `1013/MPSupportFiles-AUSTIN-PC-0445.cab`: 18,569,583-byte local diagnostics.
- `text-logs/`: only diagnostic text/metadata extracted for inspection, no sample.
- `microsoft-review-draft.md`: a concrete classification request ready for review.

The full diagnostics include machine identifiers and paths and remain local.
Preparing this report does not authorize uploading it. Microsoft's
[submission process](https://www.microsoft.com/en-us/wdsi/filesubmission) is the
remaining route to confirm/correct the verdict. User authorization and a sample
handling plan are required before sharing or restoring quarantined content.

The investigative work has not established that the binary is safe to release.
Do not change bytes/compiler options or remove features merely to find a variant
that avoids detection; such a result would not validate the flagged artifact.
Keep the final launcher blocked pending classification. The verified game import
and isolated New Game acceptance remain preserved.

## Owner restoration follow-up

The owner restored all detections. Read-only inspection then recovered the flagged
artifact SHA256:
`bb4c9d33ce05ec999e9abdace61164951f584f8286b684d6915249bf7d4e8fbc`.
All five restored copies match, so the alerts concern one duplicated artifact.
Both this executable and the earlier unflagged launcher are unsigned. The restored
PE has ordinary Windows/CRT imports and ASLR/DEP flags. These are supporting
metadata, not clearance of the Defender verdict. A public web search for the
exact hash returned no results; that does not establish a clean verdict.

The local Microsoft draft now includes the correct flagged artifact hash, and
`restored-artifact-hashes.json` records the five comparisons. No restored launcher
was executed, no investigator-created exclusion was added, and no sample uploaded.
The earlier unavailable-hash/quarantine statements describe the pre-restoration
investigation. Microsoft classification remains unresolved.
