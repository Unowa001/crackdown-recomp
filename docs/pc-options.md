# PC menu options, milestone 2

**Windows tested for opening, resolution persistence and Back (2026-10-12).** PC Settings
now opens a native game XUI page using the unused screen 26 (`options`). The
page clones DisplayOptions chrome and radio styles, has three cyclic rows,
a restart notice, and a visible native Back button. Its original B/Back binding
is retained. The unset DisplayOptions footer is hidden. The menu's interim SDK
overlay and custom F4 interception are removed; F4 keeps the SDK's normal binding.
Quit to Desktop retains milestone 1's mode-2 confirmation and clean shutdown.

Milestone 1 Windows receipt (Claude Code, 2026-10-12): Release at `2f4a14f`, owner
with a PS4 controller, log `crackdown_002`. Both entries appeared in main menu and
in-game Start; navigation, popup No/Yes, mode 2 and exit code 0 worked. The owner
found the SDK overlay unthemed and unable to close with the controller. That
receipt validates the existing menu/quit path, **not the new native screen**.

| Row / element Id | First selection (default; remove key) | Second selection |
|---|---|---|
| Frame rate / `framerate` | 60 fps: remove `vsync` | 30 fps (original): `vsync = true` |
| Internal resolution / `resolution` | 720p: remove `resolution_scale` | 1440p: `resolution_scale = 2` |
| Display / `display` | Fullscreen: remove `fullscreen` | Windowed: `fullscreen = false` |

The effective startup cvars are captured after GPU loading and ApplyGameDefaults.
Screen construction sets the three selections from that snapshot. Subsequent
successful edits stay selected when the page is reopened, until restart. Other
resolution scales start with no selection (-1); they are retained until a preset
is deliberately chosen. No setting is applied live, including fullscreen. This
avoids changing simulation/frame timing mid-session or invoking the SDK's window
callback on a guest thread. On restart, `ApplyGameDefaults()` interprets explicit
`vsync=true` as stock 30 fps and skips the measured-clock/60 fps defaults. Removing
it restores the normal 60 fps defaults unless other explicit tuning overrides them.

Each actual changed selection saves only its key into `crackdown.toml` beside
the exe. `crackdown/common/toml_edit.{h,cpp}` contains the launcher's existing
toml++ source-preserving reader/editor; both targets use it. It preserves unrelated
settings, Unicode, comments, key order and CRLF bytes, parses before replacement,
and removes default overrides. Native initialization, focus notifications and
unchanged selections never save. A failed save logs a warning, keeps the previous
pending state and restores the radio selection. It never calls `rex::cvar::SaveConfig`.
Command-line options still override the saved file on the next launch. Explicit
`fps_cap`, simulation flags and per-axis resolution overrides are preserved too;
these advanced overrides can make the displayed presets differ from actual output.

The package/runtime pipeline:

1. The hash-gated recipe regenerates the menu with the verified game-specific
   Text field ordinal (see the Windows schema receipt in re-notes.md).
   Global Scene.xzp gains only `options.xur`; every existing entry,
   including its menuHud_3.xur, remains byte-identical. Root Id `options` and class
   `scene` match the original screen registry/factory. Hidden, disabled,
   unfocusable `AgentProfiles` (label) and `AudioSetup` (navbutton) have no nav links
   or timelines. The original constructor looks up and dereferences both wrappers;
   omitting them crashes. Removed DisplayOptions controls/animations cannot reset
   the new row positions or navigation.
2. CMake deploys both `xui/menuHUD_3.xpatch` and `xui/Scene.xpatch` beside the exe,
   even when building only `crackdown`. Ship both files. Patches contain audited
   COPY/INSERT operations bound to original and result SHA256, never game assets.
3. Before guest launch, both packages are verified, reconstructed and cached at
   `<cache_root>/xui/<name>.<result hash prefix>.xzp`. Cache reuse verifies all bytes;
   reconstruction still validates the shipped patch even on a cache hit. Publishing
   uses a sibling temporary file and atomic replacement (MoveFileExW on Windows).
   Logs measure cumulative SHA256 time and total preparation time for each package.
   The 25.9 MB Scene hash cost is accepted; record its measured PC startup cost.
4. Both results are exposed through the existing read-only `pcmod:` device. Routing
   is enabled only after both packages, VFS and guest path allocations are ready.
   CreateFileA (0x82742B28) redirects only the exact global Scene/menu paths, with
   case/separator normalization. Localization packages are untouched. The original
   files are never overwritten. Missing/corrupt patches, mismatched originals or
   cache/VFS errors disable both redirects/actions with one warning. A failed
   redirected open retries the original with the original context.
5. The new pcSettings press reproduces Tab1's ShowScreen call with id 26 and
   parent `sub_8268DC40()->+4`. The original factory/constructor runs, then the
   constructor hook sets radio selections via 0x8269B5D8. Side calls use copied
   register contexts and private aligned guest stack frames.
6. ObjectProc (0x8269C3C0) observes XM_NOTIFY=14 only while the stack top is screen
   26. Every notify logs its code/element at DEBUG. Code 4 is the radio wrapper's
   change event in disassembly; its actual PC behavior is still unverified. Ids
   come from 0x826E1CA8 and selections from 0x8269C378. Initialization is guarded
   against synchronous notification saves. Original notification propagation is
   retained, including scene Back/destroy/stack handling. The focus-index helper
   0x82624198 is never called: it hangs with no focused child; any future use must
   first guard it with 0x82624120.

Literal English labels are intentional. Future localization needs
`Local/<locale>/XUIFrontEnd/Scene.xzp#options.xus` paths rooted at `options`:
pageTitle, framerateLabel, resolutionLabel, displayLabel, restartNotice,
XuiBackButton1, and each row's XuiRadioButton1/2 `.Text` (plus locale-specific
font/size metrics). The recipe generates audited prospective localization patches,
including the existing menu labels, but only the two global patches are shipped.

Regenerate/audit from the repo root using the verified reference dump:

```sh
python3 -m unittest discover -s tools/xui -v
python3 -m tools.xui.mod_menus /path/to/game --manifest tools/game-manifest.txt
python3 -m tools.xui.xpatch audit /path/to/game/XUIFrontEnd/Scene.xzp out/xui/patches/XUIFrontEnd/Scene.xzp.xpatch
cp out/xui/patches/XUIFrontEnd/Scene.xzp.xpatch patches/xui/Scene.xpatch
cmp patches/xui/menuHUD_3.xpatch out/xui/patches/XUIFrontEnd/menuHUD_3.xzp.xpatch
```

Scene audit metadata is committed in `patches/xui/Scene.audit.json`: 140
operations, 3,276 patch bytes, 1,300 literal bytes, longest INSERT 596 bytes, zero
original-matching 16-byte windows. Original: 25,914,713 bytes,
`9ca02bfedd706fce30197156edeb7aeea6751f0cdd6c111fac56ef020ae5f95d`.
Result: 25,918,624 bytes,
`a539651e377bd1a96c176079b75d0cd7ba10ec42ddd5f91a9265ba2ad3265f95`.
The standalone menu result is `9fb7a175...` with a 2,191-byte patch.

Server checks (Codex, 2026-10-13): 28 Python tests; full verified recipe/audit of
15 packages; C++ XUIPAT1 integrity tests and real Scene Python/C++ parity;
PC settings persistence tests; launcher CTest suite. All passed. Changed game/test
translation units compile-check against `sdk/include` exported from the exact
pinned 0.10.0 commit `f5337cd`; dependency files confirm no 0.9 `rex` header fallback.
SHA256 in the game and tests compiles `sdk/thirdparty/crypto/sha256.cpp`, since
0.10 does not install `rex/crypto/sha256.h`. Host integrity tests link the installed
Linux 0.9 runtime using `-DREXSDK_VERSION=0.9.0`; this is not a full Windows link/run.
No SDK source or shared generated code was changed. toml++ emits its existing
Clang deprecated literal-operator warnings.

Host checks can be reproduced with:

```sh
cmake -S crackdown -B out/pc-options-build -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 -DREXSDK_VERSION=0.9.0 -DCRACKDOWN_PC_OPTIONS_TESTS=ON
cmake --build out/pc-options-build --target xui_patch_tests pc_settings_tests
ctest --test-dir out/pc-options-build --output-on-failure
out/pc-options-build/xui_patch_tests /path/to/game/XUIFrontEnd/Scene.xzp patches/xui/Scene.xpatch out/xui/modified/game/XUIFrontEnd/Scene.xzp
cmake -S crackdown/launcher -B out/launcher-tests -G Ninja -DLAUNCHER_BUILD_UI=OFF -DBUILD_TESTING=ON
cmake --build out/launcher-tests
ctest --test-dir out/launcher-tests --output-on-failure
```

Windows test (Claude Code/owner; required before merge):

- Build `wip/pc-options` with installed SDK 0.10.0 and the usual
  `build-and-run.bat`/Release process. Confirm both patch files beside the exe.
  Start once with `--log_level=debug` to collect every new-page notification.
  Expected startup: `pc menu: ready pcmod:\Scene.a539651e.xzp`, both original/result
  hashes; `pc menu: Scene cache rebuilt` (then `reused` on second boot),
  `SHA256 ... ms; preparation ... ms`; native routing enabled. The Scene open must
  log `redirect ... (verified Scene)` before the page can be created.
- Open PC Settings from the main menu and from Start during gameplay. Expected:
  `pc menu: pcSettings -> native PC Settings (screen 26)` and
  `pc settings: initialized screen 26 (framerate=0, resolution=0, display=0; restart required)`
  for defaults. Confirm title, all labels and readable themed rows/notice, no
  overlaps, no visible dummy controls, no DisplayOptions hints. Also test a second
  locale: literal new labels must survive the global/localized resolver.
- With the PS4 controller, Up/Down must cycle all three rows, Left/Right or A must
  change their radios, and B must return to the prior page. Repeat with keyboard
  navigation and mouse clicks, including the visible Back button. Reopen the page
  repeatedly to check stack lifetime and restored pending selections. Start is
  not an extra Back shortcut on screen 26 (the native base excludes this id).
- Inspect DEBUG lines `pc settings: notify code=4 id=framerate` (also resolution,
  display). Each actual edit should emit one `pc settings: saved vsync=true`
  or corresponding key/value `(restart required)`. Focus alone must not save;
  returning to the first selection logs `default (key removed)`. If the real
  selection event differs, keep the notify log and fix the hook before acceptance.
- Toggle every option and inspect adjacent crackdown.toml: only changed keys may
  differ, unrelated comments/settings must remain. Restart and verify 30/60 fps,
  scale 1/2, fullscreen/windowed, and selected values. Confirm no live setting
  change occurs. Test a read-only/invalid config: warning, prior selection restored,
  no overwritten content. Command-line/custom tuning can override these presets.
- Recheck original Quit/Extract and Quit to Desktop No/B/Yes, clean exit/log flush.
  Test `--pc_menu_mod=false`, either missing/corrupt patch, mismatched original,
  unwritable cache, corrupt-cache rebuild and removal after startup. Original
  menus must remain available, with no repeated missing-file loop.

Record PC logs/screenshots and Claude Code's build/review receipt before merge.
Static parsing, disassembly and server checks do not prove controller navigation,
notify timing/Id scope, mouse Back behavior or native visual alignment.
