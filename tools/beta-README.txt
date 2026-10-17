Crackdown Recomp beta (Windows x64)

Requirements: x64 CPU with AVX2 and FMA3, a D3D12 GPU, Windows 10/11,
and the Microsoft Visual C++ x64 Redistributable installed.
Supply your own supported Crackdown World multi-language disc dump.
Supported default.xex SHA256:
7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a

Extract this entire ZIP into a user-writable folder, such as Documents.
Run CrackdownLauncher.exe. Select your ISO (full XGD2 or trimmed XISO),
or select a folder that already contains the extracted game files.
An existing extracted folder is used in place; it is never copied or changed.
ISO imports create game/ beside the launcher after checking every file.
A newly selected folder gets the same one-time check. "Verify all files"
repeats it later, for example after a crash that might be a damaged file.
Cancelled and failed imports remove only their temporary extraction folder.
The package contains no disc image, XEX, game assets, or generated source files.

60 fps is the default. 30 fps restores stock simulation settings.
1440p internal rendering is experimental. This build uses bilinear upscaling.
The launcher preserves custom settings in crackdown.toml. Custom fps_cap or
simulation options can override the presets; remove those overrides to use defaults.
Saves and cache use Documents\crackdown. Save profiles in the launcher keep separate
saves (Documents\crackdown\profiles\<name>); a new profile starts a fresh game.
Archipelago multiworld (experimental): download crackdown.apworld from the same
release and open it with Archipelago 0.6.8 or newer (or copy it into the
Archipelago custom_worlds folder). Make your player YAML with the Options Creator
or start from Crackdown.yaml. Whoever generates the seed needs the apworld; host
the generated zip on archipelago.gg (Upload) or with ArchipelagoServer.
In the launcher, create a new save profile for each seed, open the ARCHIPELAGO
tab and enter the server (for example archipelago.gg:38281), your slot name and
the room password, then Play. The game connects by itself; no separate client.
A profile's save is bound to the first seed it connects to; other seeds are refused.
Checks: agility and hidden orbs, supply points, weapon deposits, bosses.
Items: Progressive skills, weapons, Agency Commendation (filler).
Goal: defeat the three gang kingpins in your own game.
Optional: set crackdown_options.launcher_path in Archipelago's host.yaml so the
"Crackdown Client" entry and archipelago:// room links open this launcher.
Open logs folder and Copy system info help prepare a bug report. Include the
latest crackdown_NNN.log, VERSION.txt, what happened, and reproduction steps.

Third-party notices are in LICENSES. The exact revision and SDK patch hashes
are under "Build details" at the end of this file.
Setup guide and releases: https://github.com/Unowa001/crackdown-recomp
Unofficial fan project, not affiliated with or endorsed by Microsoft. It contains
no game data; you must own the game and supply your own disc dump.
