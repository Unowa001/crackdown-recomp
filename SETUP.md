# Setup

## What you need

- Windows 10 or 11, 64-bit.
- A CPU with AVX2 and FMA3 (most Intel since 2013 / AMD since 2015) and a DirectX 12 GPU.
- The [Microsoft Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- Your own dump of the **Crackdown World multi-language disc** (an ISO, full XGD2 or trimmed
  XISO, or a folder of already-extracted files). The launcher checks every file against the
  supported version (`default.xex` SHA256 `7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a`).

## Install

1. Download `CrackdownRecomp-beta<version>.zip` from the
   [latest release](https://github.com/Unowa001/crackdown-recomp/releases/latest).
2. Extract the **whole** ZIP into a folder you can write to, e.g. `Documents\CrackdownRecomp`.
   Don't use `Program Files`, and don't run it from inside the ZIP.
3. Run `CrackdownLauncher.exe`.
4. Choose **Install game** and select your ISO (or a folder with the extracted game).
   The launcher extracts it into `game\` next to itself and checks every file. This takes a
   minute or two. An existing folder you select is used in place and never modified.
5. Press **Play**.

### Windows warnings (expected for this beta)

The files are not code-signed, so Windows may warn you:

- **SmartScreen** ("Windows protected your PC"): click **More info → Run anyway**.
- **Microsoft Defender** has flagged earlier builds of the launcher as
  `Trojan:Win32/Bearfoos.A!ml`. This is a machine-learning ("!ml") heuristic match on an
  unsigned new program; we believe it is a false positive. If it happens:
  open **Windows Security → Virus & threat protection → Protection history**, select the
  entry and choose **Restore** (or **Allow on device**). If you are unsure, don't run it, and
  tell us instead.

## Playing

- Controller recommended (Xbox-style controllers work out of the box).
- 60 fps is the default. In the launcher's settings, **30 fps** restores the original timing.
- **Save profiles** in the launcher keep separate saves; a new profile starts a fresh game.
- Saves, settings and logs live in `Documents\crackdown`.
- Startup shows a few logo screens for about 25 seconds (skipping them is planned).

## Updates

The first time you open the launcher it asks whether to check for updates. You can also
use **Check for updates** at any time. Updates download from this repository's releases,
are verified (SHA256) and replace only the program files. Your game files, saves and
settings are kept, and a failed update rolls back.

## Reporting a problem

In the launcher, use **Open logs folder** and **Copy system info**. Send:
the newest `crackdown_NNN.log`, the copied system info, `VERSION.txt` from the install
folder, what you were doing, and whether it happens again.

## Archipelago randomizer

In development and **not part of this beta's normal setup**. A future update will add it.
