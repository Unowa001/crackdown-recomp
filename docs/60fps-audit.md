# 60 fps audit (2026-10-12)

Output of `tools/fpsaudit.py` (guest image from `--dump_image`, generated code from SDK v0.10 + patches 0001-0006).
Each line lists a function and what it touches. `name@addr` is a float constant load, `CLOCK+n` is a field of the game
clock object `[0x82DE25C0]`, `VARFLAG` is 0x82DE3FB0, `STEP` is 0x820ED704, `C1/60` is 0x820ED634, `FRAMECAP` is 0x82BAA330.
`30`/`60` hits are mostly angles or unit conversions, so they're noise until checked.

Checked so far:
- VARFLAG readers outside the 13 hooked timer sites (sub_821422D0, sub_82604560, sub_824C83D0, ...): `time = VARFLAG ?
  [0x82D9912C] : GetTickCount()`, a virtual-timer switch. Real time while the flag is clear, so OK at 60 fps.
- sub_8228E130: step / (k * 1/60) = 60 Hz frames elapsed. Frame-rate aware, OK.
- sub_82330888: anim time += rate * 1/30 (own constant 0x820ED7CC), wrapped at clip length. **Plays 2x fast at 60 fps.**
  Packed-float16 anim data (crowd/NPC?), not the player. To fix later.
- Stairs speed bug at 60 fps (crackdown_024, `--fps_cap=60 --sim_game_clock`, post-0006): **found and fixed 2026-10-12.**
  The step-down snap sub_822B4D40 sets `out.y = -drop * gain / dt`; at 60 fps the pull is twice as strong and the
  character proxy's solver converts it into forward speed on stair edges. `--sim_ground_snap_30hz` (default on) gives it
  the 30 fps velocity. Replay A/B from a teleport: peak 11.86 m/s at 30 fps, 24.41 at 60 unfixed, 11.96 fixed
  (crackdown_053/054/055). See docs/re-notes.md "Player character".
- **Havok world step** (measured with `--diag_physics_dt`, `crackdown/src/hooks_diag_physics.cpp`, title screen runs on this PC
  2026-10-12): game physics update sub_8248F210 (one of the 13 clock sites) -> sub_82492118(dt) -> hkWorld::stepDeltaTime
  sub_82980AE0 (call at 0x82980B0C) -> simulation step sub_829BD048(dt). dt = clock ms * 0.001 (constant at 0x82070000+8912).
  - `--fps_cap=60 --sim_game_clock --sim_game_clock_sites=0` (crackdown_028): dt = 0.033 every frame while the step is 0.016,
    so **physics runs at 2x speed**. That matches T2 (fast cars, sharp turns, NPC cars into walls, stairs).
  - `--fps_cap=60 --sim_game_clock` (all sites, crackdown_029): dt follows the measured frame (0.016-0.020). Physics speed OK.
- 0x820ED704 is not a dedicated step variable: it is the binary's pooled 1/30 literal. Havok's hkWorldCinfo constructor
  sub_8298D618 also reads it (default expectedMinPsiDeltaTime = 1/30), as do the other readers listed below. Patching it
  changes all of them.
- hkWorldCinfo::m_expectedMinPsiDeltaTime (+92, from Havok's reflection table) is filled from the pooled 1/30 by sub_8298D618.
  Under --sim_game_clock it got the frame time at load (0.016/0.017, crackdown_033). `hooks_framerate.cpp` now restores 1/30,
  but the stairs bug is unchanged (user, crackdown_033). Not the cause.
- Physics speed was fine in crackdown_031/033 (dt = frame time); the user still saw driving "a little fast" and cars totaled
  instantly hitting the bottom of the stairs. Unexplained.
- **Vehicles** (2026-10-12, `--diag_vehicle` on the game's vehicle step sub_824A1708, replayed garage drive and stairs drive,
  see docs/re-notes.md "Vehicles"): with `--sim_game_clock --sim_step_literal` acceleration, top speed, impact size and damage
  match 30 fps. Without `--sim_step_literal` the car accelerates faster (the "a little fast" report). The "car totaled at the
  bottom of the stairs" report (crackdown_024/033, no `--sim_step_literal`) did not reproduce in any configuration.
- sub_823FF3B8: t += 0.033 (0x82CFF2B4) per frame, then fmod. Cosmetic timer, runs 2x at 60 fps.
- **Director VO / wave safety timeout** (2026-10-14): `0x8225AB30` initializes
  `deadline = floor(samples/48000)*30+70`; `0x8225B254` advances elapsed by one
  per update, and `0x8225B274` calls source Stop when elapsed exceeds deadline.
  `--sim_audio_timeout_30hz` (60 fps default, user override wins) retimes only
  that increment to the measured simulation step, with fractional carry per
  wave instance. Stock `--vsync=true` leaves the hook disabled. `VOA_Eng[656]`
  (5282.6 ms) was cut at 3671 ms with pending XMA input/output; fixed 60 fps
  runs complete at 5367/5369 ms versus 5401/5402 ms at stock 30, with empty
  rings. Whole anonymous SFX bucket totals still differ across frame rates;
  same-clip completion is proven, full distribution equivalence is not.
  Addresses, disassembly, all bucket tables and log receipts: `re-notes.md`,
  "Director VO timeout investigation". Audible confirmation remains with owner.

```
sub_82142158 30@82072038
sub_821422D0 VARFLAGx2
sub_82148040 30@82072038
sub_821485F8 30@82072038
sub_82148770 30@82072038
sub_8214A900 30@82072038 60@82072DDC
sub_82153F08 30@82072038
sub_82155F38 30@82072038
sub_8215B8B0 30@82072038
sub_8215CAD8 30@82072038
sub_82160028 60@82072DDC 60@820ED768
sub_82161C80 30@82072038
sub_82191750 30@82072038
sub_821A0C90 30@82072038
sub_821A48A8 STEPx4
sub_821AB8A0 STEPx2
sub_821AC810 30@82072038
sub_821AFF40 30@82072038
sub_821B0550 30@82072038
sub_821B4C58 0.03@820ED5E0
sub_821BA138 60@82072DDC
sub_821BBC88 30@82072038 60@82072DDC
sub_821C4E78 30@82072038
sub_821CC938 30@82072038
sub_821E38E0 30@82072038 33@820ED548 60@82072DDC
sub_82201230 60@82072DDC
sub_82209F20 30@82072038
sub_8220C998 60@82072DDC C1/60x2
sub_8221BEC0 30@82072038
sub_82250080 30@82072038
sub_82250F90 30@82072038
sub_82256C00 0.03@820ED5E0
sub_82257668 30@82072038x3
sub_82265C70 STEPx2
sub_8226BAB8 2/30@82072048x2
sub_8227A108 30@82072038 33@820ED548
sub_8227B350 60@82072DDC
sub_82287130 60@82072DDC CLOCK+13 VARFLAGx2
sub_8228A3D0 0.03@820ED5E0
sub_8228B0F0 STEPx2
sub_8228E130 C1/60x2
sub_822934D0 30@82072038
sub_822A2C90 STEPx2
sub_822A9E78 30@82072038x2
sub_822B3530 0.03@820ED5E0
sub_822B4D40 0.03@820ED5E0
sub_822BE450 C1/60x2
sub_822C1690 30@82072038
sub_822D16F0 0.03@820ED5E0 30@82072038
sub_822D3220 0.03@820ED5E0
sub_822E1F10 60@82072DDC
sub_822E6D60 30@82072038
sub_822E8840 0.03@820ED5E0
sub_822E8DF8 0.03@820ED5E0
sub_822F3568 60@82072DDC
sub_822F4900 0.03@820ED5E0
sub_82305018 C1/60x2
sub_82307D88 0.03@820ED5E0
sub_8231B190 CLOCK+16
sub_82320CE8 VARFLAGx2
sub_82322F28 CLOCK+0
sub_823238A8 CLOCK+0
sub_82324330 CLOCK+14x5 CLOCK+16
sub_82324938 CLOCK+13 CLOCK+14x2 CLOCK+16 CLOCK+16.w
sub_82325850 CLOCK+14.w
sub_82326348 VARFLAG VARFLAG.w
sub_823263D8 VARFLAG VARFLAG.w
sub_8232BDD8 FRAMECAPx4 FRAMECAP.wx2
sub_8232BE20 FRAMECAPx4 FRAMECAP.wx2
sub_8232BE68 FRAMECAPx6 FRAMECAP.wx2
sub_82330888 1/30@820ED7CC
sub_82330B68 2/30@82072048 STEPx2
sub_82338220 2/30@82072048 STEPx2
sub_82340C58 30@82072038
sub_82342470 30@82072038 STEPx2
sub_82354560 C1/60x2
sub_8235D570 30@82072038
sub_8236CD38 30@82072038
sub_82388D90 CLOCK+13 VARFLAGx2
sub_82389620 CLOCK+16.w
sub_823A5480 0.03@820ED540 30@82BA8F88
sub_823A84D8 0.03@820ED5E0
sub_823A9898 0.03@820ED540
sub_823AB038 0.03@820ED5E0
sub_823B51F0 CLOCK+14
sub_823C4018 CLOCK+14
sub_823C6C88 30@82072038
sub_823D0508 60@820ED768
sub_823DC448 60@82072DDC
sub_823DC908 30@82072038
sub_823DE7F8 60@82072DDC
sub_823E0890 30@82072038
sub_823E0F70 30@82072038
sub_823E1058 30@82072038
sub_823E16E8 30@82072038
sub_823E1AA0 30@82072038
sub_823E7480 30@82072038
sub_823E7590 60@82072DDC
sub_823EC0D0 60@82072DDC
sub_823ECCB0 60@82BA8DFC
sub_823F0E70 CLOCK+13 VARFLAGx2
sub_823F7D00 60@82072DDC
sub_823FBC38 2/30@82072048
sub_823FD6A8 33@820ED548
sub_823FF3B8 0.033@82CFF2B4
sub_823FFD28 VARFLAGx2
sub_8241EA60 60@82072DDC
sub_824345F0 30@82072038
sub_82438BE8 30@82072038
sub_8243D650 30@82072038
sub_82440D38 30@82072038
sub_82465C50 CLOCK+16x2
sub_82466AC0 CLOCK+16
sub_824699A0 CLOCK+14.w CLOCK+16.w
sub_8246A478 CLOCK+16
sub_8246A5C0 CLOCK+16
sub_8246A828 CLOCK+16
sub_8246AB78 CLOCK+16
sub_8246B890 CLOCK+16
sub_8248F210 CLOCK+13 VARFLAGx2
sub_82493C00 30@82072038
sub_8249FD68 60@82072DDC
sub_824A6908 60@82072DDC
sub_824A8EE0 30@82072038 60@82072DDC
sub_824C7360 0.03@820ED5E0
sub_824C83D0 VARFLAGx4
sub_824CB0E0 30@82072038
sub_824D62C0 30@82072038
sub_824ED730 30@82072038
sub_824F9518 0.03@820ED5E0 CLOCK+13 VARFLAGx2
sub_824FAC28 0.03@820ED5E0 CLOCK+13 VARFLAGx2
sub_82505CB8 CLOCK+13 VARFLAGx2
sub_8251F770 60@82072DDC
sub_8251F7D8 33.33@8206F2F8
sub_82528598 30@82072038
sub_8252EDA0 STEPx2
sub_82531228 2/30@82072048
sub_8253B078 CLOCK+16
sub_8254C568 C1/60x2 CLOCK+13 STEPx2 VARFLAGx2
sub_8255BC68 2/30@82072048
sub_825668D8 60@82072DDC
sub_8256E8B8 60@82072DDCx2
sub_8257A4F8 CLOCK+13 VARFLAGx2
sub_82586D28 33@820ED548
sub_82586F30 33@820ED548
sub_82596F20 2/30@82072048
sub_8259A330 CLOCK+13 VARFLAGx2
sub_825B5728 0.03@82BA7154
sub_825BFE80 60@82072DDC
sub_825C80D0 30@82072038 60@82072DDC
sub_825CD930 CLOCK+14
sub_825D1EC0 2/30@82072048
sub_825D74A8 60@82072DDC
sub_825DB500 30@82072038
sub_825E11D0 CLOCK+13 VARFLAGx2
sub_825F01E8 33.33@8206F2F8
sub_825F1228 33.33@8206F2F8
sub_82604560 VARFLAGx6
sub_82605268 VARFLAGx6
sub_82608270 VARFLAGx2
sub_82608368 VARFLAGx2
sub_82624D78 CLOCK+14
sub_82650818 CLOCK+14
sub_82651570 CLOCK+13 VARFLAGx4
sub_82660C20 30@82072038 60@82072DDC
sub_826724E0 VARFLAGx2
sub_82672A70 VARFLAGx2
sub_826734C8 VARFLAGx2
sub_826739B8 VARFLAGx2
sub_82673D50 VARFLAGx2
sub_82674040 VARFLAGx2
sub_82677258 CLOCK+14 CLOCK+16
sub_826A65F0 FRAMECAPx2
sub_826A6B50 CLOCK+14 CLOCK+14.w
sub_826A7698 CLOCK+0 CLOCK+13x2 FRAMECAPx2 STEPx2 VARFLAGx2
sub_826A7AD0 CLOCK+16
sub_826D0AF8 30@82072038 60@82072DDC
sub_826DF028 60@82072DDC
sub_826E9808 30@82072038 60@82072DDC
sub_826F76B8 33.33@8206F2F8
sub_8271BD58 30@82000B80 60@82000BBC
sub_8271C1A8 C1/60x2
sub_8271C540 C1/60x2
sub_8271CE30 C1/60x2
sub_8272AEA0 60@82000BBC
sub_8273F798 30@82000B80
sub_82759CB8 2/30@82005058
sub_8275A358 2/30@82005058
sub_8275C878 2/30@82005058
sub_8275F248 2/30@82005058
sub_8275FAD8 2/30@82005058
sub_8275FEB0 2/30@82005058
sub_827929D0 2/30@82005058
sub_82792E50 2/30@82005058
sub_828E7810 2/30@82005058
sub_828E7EB0 2/30@82005058
sub_828EA658 2/30@82005058
sub_828EC6B0 2/30@82005058
sub_828ECF40 2/30@82005058
sub_828ED3E8 2/30@82005058
sub_82914A58 2/30@82005058
sub_82914E68 2/30@82005058
sub_8297FF68 2/30@82049FB8
sub_8298D618 STEPx2
sub_8299E9F0 33@820ED548
sub_829D5C58 C1/60x2
sub_829D69F0 C1/60x2
sub_82A06D00 30@82000B80
sub_82A0E030 60@82000BBC
sub_82A13778 60@82000BBC
sub_82A595A8 30@82000B80
sub_82A76770 33.33@82058838
sub_82A90AA8 30@82000B80
sub_82A95780 30@82000B80
sub_82A97900 30@82000B80
sub_82B45A78 C1/60x2
sub_82B46608 C1/60x2
```
