# XUIZ package format used by the launcher

The full XUI research documentation and Python reference were read from the
sibling `wip/xui-tools` worktree (`docs/xui-format.md`, `tools/xui/xzp.py`),
which was not present on this branch at the start of launcher v2 work. This
page records the package subset needed by this launcher; it does not describe
or implement XUR scenes, localization, menu editing or game patches.

All integers below are unsigned and big-endian. Names are UTF-16BE without a
terminator. The observed packages have contiguous, uncompressed payloads.

| Offset | Field |
| --- | --- |
| 0x00 | Four bytes `XUIZ` |
| 0x04 | u32 version, 1 |
| 0x08 | u32 complete file size |
| 0x0c | u32 reserved, 0 |
| 0x10 | u32 entry-table size, excluding the 22-byte header |
| 0x14 | u16 entry count |
| 0x16 | Entry table |

An entry has `u32 payload_size`, `u32 payload_offset`, `u8 name_units`,
then `name_units * 2` name bytes. Offsets are relative to the payload start,
`0x16 + table_size`. Entries and payloads are in the same order, without
padding or compression. The table must end exactly at that boundary and the
last payload must end exactly at the advertised complete file size.

The C++ reader checks magic, version, reserved value, total size, all table and
payload bounds, contiguity, complete consumption and UTF-16 surrogate pairs.
Duplicate entries remain in the result, as they exist in some real packages;
a lookup for an ambiguous duplicate name fails. A missing lookup returns null.
Nothing is extracted to an entry-controlled filesystem path.

After the core verifies the selected game, the art loader reads
`XUIFrontEnd/Scene.xzp` and hashes the actual buffer against its trusted
manifest row before decoding any PNG. It looks up these exact filenames, including nested XUI entries when the
filename is unique:

- `AgencyBuildingRotation.png`: backdrop.
- `LogoCrackdown.png`: logo.
- `Agency-logo-big.png`: accent (the real dump stores this as
  `xui-assets-3\Agency-logo-big.png`).

Each missing or undecodable image falls back independently. The decoded RGBA
cache is bounded and keyed by the source SHA256 under the user data folder's
`launcher-art/`. The original package is never written. Game art and derived
cache files are never package inputs or checked-in assets.

`launcher_xzp_reader` uses only a synthetic package. It checks truncation,
malformed fields, entry lookup, missing and duplicate names, and Unicode
surrogates. Real-disc decoding and the visual composition still require the
Windows acceptance checks in `docs/launcher-v2.md`.
