# Crackdown Recomp (beta)

An unofficial native Windows PC port of **Crackdown** (Xbox 360, 2007), made by static
recompilation. It runs at 60 fps by default and includes a launcher that installs the
game from **your own** disc dump, checks your files and updates itself.

- **Experimental:** two-player [co-op over System Link](SETUP.md#co-op-experimental) and an
  [Archipelago randomizer](SETUP.md#archipelago-randomizer-experimental).
- **Setup guide:** [SETUP.md](SETUP.md)
- **Download:** [latest release](https://github.com/Unowa001/crackdown-recomp/releases/latest)

This repository holds the source, releases and documentation. It contains **no game data**
and no generated code: you must
own Crackdown and supply your own dump of the disc. Unofficial fan project, not affiliated
with or endorsed by Microsoft.

## Source

- `crackdown/src`: game hooks and runtime fixes on top of the
  [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (submodule `sdk/`, pinned).
- `patches/sdk`: SDK fixes, applied by `build-and-run.bat` / `tools/apply-sdk-patches.sh`.
- `crackdown/launcher`: the launcher and installer.
- `docs/`: reverse-engineering notes, the 60 fps audit and the build playbook
  (`docs/playbook.md`). The recompiled C++ is generated locally from your own `default.xex`.

## License

BSD-3-Clause, see [LICENSE](LICENSE). Third-party code keeps its own notices
(`tools/launcher-NOTICES.txt`, `sdk/LICENSE`).
