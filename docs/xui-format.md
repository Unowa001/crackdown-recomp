# Crackdown XUI packages and scenes

This describes the files in `game/XUIFrontEnd/` on the supplied 2007 disc dump.
It is format research for a later load-time menu patch; no runtime hook or game
asset is included here. All offsets below are hexadecimal, relative to the
**extracted file**, unless explicitly called package offsets. Integers and
IEEE-754 binary32 values are big-endian. Strings are UTF-16BE, with lengths in
UTF-16 code units, without a terminator.

## Tools and reproducibility

Python 3.10+ standard library only; no SDK or third-party parser is needed.
Run from the worktree root:

```sh
python3 tools/xui/xzp.py list ~/DecompProject/game/XUIFrontEnd/Scene.xzp
python3 tools/xui/xzp.py roundtrip ~/DecompProject/game/XUIFrontEnd/Scene.xzp
python3 tools/xui/xzp.py extract ~/DecompProject/game/XUIFrontEnd/Scene.xzp out/xui/Scene
python3 tools/xui/xur.py dump out/xui/Scene/menuHud_3.xur
python3 tools/xui/xur.py json out/xui/Scene/menuHud_3.xur
python3 tools/xui/xur.py roundtrip out/xui/Scene/menuHud_3.xur
python3 -m unittest tools.xui.test_xui -v
python3 tools/xui/audit.py ~/DecompProject/game/XUIFrontEnd \
  --locale-archive ~/DecompProject/game/Local/en-US/XUIFrontEnd/Scene.xzp
python3 tools/xui/xur.py dump out/xui/Scene/menuHud_3.xur \
  --strings out/xui/localization/en-US/Scene/menuHud_3.xus
```

Extraction is restricted to this worktree's `out/xui/`. Paths with traversal,
absolute/drive prefixes, escaping symlinks or conflicting duplicate names are
rejected. Repeated names with identical bytes and existing identical outputs
are accepted. Existing different files are never overwritten by extraction.
The audit regenerates text dumps and `out/xui/report.json`; redirect CLI dumps
and JSON only under `out/xui/` too. `out/`, XUI binaries and Python bytecode are
ignored by git.

Importable APIs:

- `xzp.read(path)` / `xzp.parse(bytes)` return ordered `Entry(name, data, offset)`
  objects, retaining duplicates. `xzp.get(entries, name)` returns bytes for a
  unique exact name. `xzp.build(entries)` accepts these objects or ordered
  `(name, bytes)` pairs and returns package bytes.
- `xur.read(path)` / `xur.parse(bytes)` return a `Scene` with section records,
  string/vector/quaternion/custom tables, `root`, `errors`, `fully_parsed`,
  `as_dict()` and `serialize()`. `xur.dump(scene)` returns text.
- `xus.parse(bytes)` / `xus.read(path)` return localization overrides with their
  offsets, values and property paths; `xus.build(overrides)` returns bytes.

`dump` and `json` exit 0 on a complete structural parse, 2 on a partial parse,
1 on an invalid header or I/O error. Unknown masks/classes stop semantic
parsing and expose the whole DATA section as raw hex; there is no invented
property length or heuristic resynchronization. Unknown sections remain raw.
`roundtrip` exits 2 for opaque preservation, even when the bytes match.

## Validation results (2026-10-12)

All 12 requested packages rebuilt byte-identically from their ordered entries.
All **152 embedded XUR occurrences, representing 110 distinct binaries**, fully
parsed and rebuilt byte-identically. No scene failed. Every XUR has version 5
and tool marker 12. The audit checked input hashes again after processing and
found every input unchanged. Dumps are `out/xui/<package>/<scene>.txt`, retaining
subdirectories in entry names; the full per-file report stays in ignored output.

| Package | Entries | XURs fully parsed / round-tripped |
| --- | ---: | ---: |
| Error.xzp | 11 | 1 / 1 |
| Fade.xzp | 1 | 1 / 1 |
| Scene.xzp | 697 | 110 / 110 |
| XUI_DistrictMap.xzp | 75 | 14 / 14 |
| XUI_MainMap.xzp | 83 | 14 / 14 |
| agencyGaragePopup.xzp | 19 | 1 / 1 |
| agencyNode.xzp | 44 | 1 / 1 |
| auto_save_game.xzp | 12 | 1 / 1 |
| dossier-detail.xzp | 20 | 1 / 1 |
| dossier.xzp | 25 | 5 / 5 |
| menuHUD_3.xzp | 14 | 2 / 2 |
| windowedVideo.xzp | 12 | 1 / 1 |

Across occurrences: 2,748 elements, 1,224 named frames, 1,316 timelines and
8,038 keyframes. Section occurrence counts are STRN=152, DATA=152, VECT=99,
QUAT=11 and CUST=17. Eleven files have the extended count header; all ten
counts match decoded records in every such file. No KEYP, KEYD, NAME or NAMD
section occurs in these v5 files. Animations are inline in DATA.

As additional localization evidence, the English locale Scene.xzp and all
110 `.xus` companions round-tripped too. Other locales and GameStrings.str
were located, but their contents were not audited. Twelve synthetic unit tests
exercise decoding, real scalar/table/keyframe edits, index/mask/count failures,
leaf animations, truncation, Unicode names, duplicates and extraction safety.
These tests contain no game bytes.

“Fully parsed” means all bytes in the known section layouts are accounted for,
including property values and animations. It does not prove that every
navigation target exists or that the game will accept an edited scene.

## XUIZ v1 package

| Offset | Field |
| --- | --- |
| 00 | Four-byte magic `XUIZ` |
| 04 | u32 version, 1 |
| 08 | u32 total file size, including header |
| 0c | u32 reserved, 0 in all 12 |
| 10 | u32 entry table size, excluding the 22-byte header |
| 14 | u16 entry count |
| 16 | Variable-length entry table |

Each entry is `u32 size, u32 payload_offset, u8 name_units, name_units*2 bytes`.
Payload starts at `0x16 + table_size`. Offsets are relative to that start;
entries and payloads are in the same order, contiguous, without alignment,
compression, checksums or padding in the observed packages.

Evidence: Fade.xzp has one entry, table size 0x19, payload start 0x2f,
406 bytes of payload and total size 453. Scene.xzp has 697 entries, table size
0xd4e1, payload start 0xd4f7 and total size 25,914,713. Every table ends exactly
at its advertised boundary and every final payload ends at the file size.
The map packages repeat some asset names, always with identical content;
these are separate contiguous entries, not aliases to the same payload offset.
The builder retains their order and duplication. Non-contiguous or padded
layouts are outside the proven format and are rejected rather than silently
normalized.

## XUIB v5 header and sections

| Offset | Field |
| --- | --- |
| 00 | Four-byte magic `XUIB` |
| 04 | u32 version, 5 |
| 08 | u32 flags: bit 0 enables extended counts |
| 0c | u16 tool marker, 12 |
| 0e | u32 total file size |
| 12 | u16 section count |
| 14 | Optional ten u32 counts, then section descriptors |

A descriptor is `four-byte tag, u32 absolute_offset, u32 byte_size`. Descriptor
and section order are preserved. Section gaps and trailing bytes are retained
by the writer, though no gap is needed by these files. Bounds, overlaps,
duplicate tags and unconsumed DATA bytes are checked.

The optional ten counts, in order, are:

1. Element records.
2. Property records, recursively including compound records and array values.
3. Property arrays: one per element with properties, plus one per compound.
4. Animated property values across all keyframes.
5. Sum of animated property path depths.
6. Animated property definitions.
7. Keyframes.
8. Timelines.
9. Named frames.
10. Elements with children.

Evidence: controllerOptions.xur has flags=1 at 08, counts at 14..3b, descriptors
at 3c and STRN at 6c. Its counts are 87, 949, 159, 358, 70, 70, 237, 47, 16,
17; each is independently recomputed. skin_default.xur has five sections and
STRN at 78; its element/keyframe/timeline counts are 943/2,794/336.
Fade.xur has flags=0, three descriptors at 14 and STRN at 38.

### STRN, VECT, QUAT and CUST

STRN is a succession of `u16 units, UTF-16BE bytes`, without a count or offset
table. Index **0 is implicit empty string**, not a serialized entry. Entries
start at index 1. Every class name, Id, reference path and literal string
property uses this table. These are table indices, not localization IDs.
Evidence: Fade.xur STRN at 38 starts with its canvas class, then scene class;
DATA at 10e starts with class index 1. Its first scene at 123 uses index 2.

VECT is packed triples of binary32 values, 12 bytes each. QUAT is packed
quadruples, 16 bytes each. References are u32 **zero-based indices**, not byte
offsets. Component interpretation is position/scale/pivot XYZ or quaternion
components; quaternion rotation convention has not been tested in a renderer.
Evidence: DisplayOptions.xur VECT at 51a has 16 triples (size c0).
skin_default.xur QUAT at 82fc has 13 quadruples (size d0); its figure `dial`
references quaternion 0 through the Rotation property at babc.

CUST here contains figure geometry, not class-registration metadata. Each block
is `u32 payload_bytes`, then two binary32 bounding dimensions, `u32 point_count`
and `point_count` groups of six binary32 values. A group encodes the point and
two Bezier control points as three XY pairs. The exact control-point ordering
and bounding-dimension meaning are reference-derived and not renderer-tested;
all numeric values and bytes are preserved. A Points property is a u32 offset
**relative to CUST**, pointing to the block's length field. Evidence:
controllerOptions.xur has one block at 12c8, length 108, four points, ending
at 1338; its Points reference at 16f3 is offset 0. skin_default.xur begins
CUST at 83cc; the next block is at 843c, consistent with 4+108 bytes.

## DATA tree and properties

Each element begins `u16 class_string_index, u8 flags`:

- Bit 0: properties follow.
- Bit 1: u32 child count, followed by recursive child records.
- Bit 2: named frames and, when there are children, timelines follow **after**
  the children.

Other flag bits are unsupported. A canvas is the outer root in every file;
scenes and visual definitions live below it. The parser keeps each node's
absolute offset, size, class index, flags, Id, property groups and children.
Evidence: Fade.xur root at 10e has properties and children; its scene at 123
and nested scene at 15c demonstrate recursive structure. menuHud_3.xur root
at 900 and tab scene at 933 demonstrate nested animated records.

Properties begin with a u16 count. There is then one group for **each class
in the inheritance chain, base first**, including classes with no own fields.
A group begins with a u8 packed descriptor:

- 0 means no properties for this class and no mask bytes.
- Low three bits give mask-byte count.
  The game's reader 0x826FCC50 accepts only 0, 1, 2 or 4 bytes. An editor
  must round a 3-byte integer mask up to 4; other nonzero widths skip values
  and fail the element's declared count (Windows logs 007..010).
- Upper five bits give the number of set properties minus one.
- Mask bytes represent a big-endian integer; bit `n` selects the class's
  **own property ordinal n**. Values follow in ascending bit order.

Global XML property IDs are not these ordinals. Counts and masks are validated.
Evidence: Fade.xur root's element group at 113 is descriptor 0a, mask 0006:
two values, Width and Height. Its canvas group is empty. Scene element/control/
scene groups at 123 onward show the same ordering. menuHud_3.xur's
controllerSettings properties at ae1 onward independently resolve Id,
dimensions, Position, ClassOverride, Visual, NavUp/Down and PressPath.

Wire value encodings:

| Kind | Bytes / interpretation | Evidence |
| --- | --- | --- |
| String | u16 STRN index, 0 means empty | menuHud_3.xur PressPath at b0b |
| Float | binary32 | Fade.xur Width at 116 |
| Integer / unsigned | i32 / u32 | InGame.xur PressKey at 29b (u32) |
| Bool | one byte | InGame.xur hidden Pause Button Show at 292 |
| Vector / quaternion | u32 table index | controllerOptions.xur Position at 140f; skin_default Rotation at babc |
| Color | four bytes ARGB, shown as `#aarrggbb` | skin_default StrokeColor at af38 |
| Custom | u32 CUST-relative block offset | controllerOptions.xur Points at 16f3 |
| Compound | u16 value count, u8 hierarchy marker, class-sized mask, values | skin_default Stroke at af34, Fill at af3c |
| Indexed array | u8 value count, then consecutive values | Gradient StopColor/StopPos in skin_default |

A compound is a Figure Stroke, Fill or nested Fill Gradient. The hierarchy
marker is retained; it is 1 in the observed compound headers. Mask length
is `ceil(own_field_count/8)`, at least one. Arrays contribute their number of
values to the compound count. Do not mistake a nested compound's child count
for the parent's field count.

The executable schema is `tools/xui/schema.py`. Relevant own-field ordinals:

| Class | Ordinals (type abbreviations below) |
| --- | --- |
| XuiElement | 0 Id:s, 1 Width:f, 2 Height:f, 3 Position:v, 4 Scale:v, 5 Rotation:q, 6 Opacity:f, 7 Anchor:u, 8 Pivot:v, 9 Show:b, 10 BlendMode:u, 11 DisableTimelineRecursion:b, 12 DesignTime:b, 13 ColorWriteFlags:u |
| XuiControl : Element | 0 ClassOverride:s, 1 Visual:s, 2 Enabled:b, 3 UnfocusedInput:b, 4 NavLeft:s, 5 NavRight:s, 6 NavUp:s, 7 NavDown:s, 8 Text:s, 9 PointSize:f, 10 ImagePath:s |
| XuiScene : Control | 0 DefaultFocus:s, 1..4 transition names:s, 5 InterruptTransitions:u, 6 IgnorePresses:b |
| XuiButton : Control | 0 PressKey:u |
| XuiNavButton : Button | 0 PressPath:s, 1 StayVisible:b, 2 SrcTransIndex:u, 3 DestTransIndex:u |
| XuiList : Control | 0 Wrap:b |
| XuiCommonList : List | 0 ItemsText:s, 1 ItemsImage:s, 2 ItemsNavPath:s |
| XuiCheckbox : Control | 0 PressKey:u |
| XuiListItem : Checkbox | 0 Layout:u, 1 Checkable:b, 2 SelectedSize:v, 3 KeepSizeUnfocused:b, 4 InterItemSpacing:v, 5 SmoothScroll:b, 6..8 scroll rates:f |
| XuiTabScene : Scene | 0 TabCount:u, 1 Wrap:b, 2 UserInterrupt:b, 3 VerticalTabs:b, 4 NoAutoHide:b, 5 DefaultTab:u |
| XuiFigure : Element | 0 Stroke:o, 1 Fill:o, 2 Closed:b, 3 Points:r |

`s`=string, `f`=float, `v`=vector, `q`=quaternion, `u`=unsigned, `b`=bool,
`o`=compound, `r`=custom reference. The source also defines the presenter,
text, image, nine-grid, slider, edit, HTML and other observed class layouts.
Schema fields that do not occur in the audited files are public-reference
candidates, **not verified for this game's SDK revision**. No property default
is synthesized when a mask omits it.

Windows startup dump `crackdown_012.log` through metadata getter 0x826D8F18
verifies the tables above: Element has 14 fields, Control 11, Scene 7,
Button 1, and Label has none. The earlier public-reference schema inserted
NavTabForward/NavTabBackward ahead of Text and treated later SDK fields as
available. That shifted Text/PointSize/ImagePath ordinals and broke generated
captions. These five schemas now match this game's runtime registrations.

### Class names and overrides

Observed serialized classes are XuiCanvas, XuiScene, XuiTabScene, XuiVisual,
XuiGroup, XuiFigure, XuiButton, XuiNavButton, XuiBackButton, XuiRadioButton,
XuiRadioGroup, XuiSlider, XuiLabel, XuiCommonList, XuiListItem, XuiEdit,
XuiHtmlControl, XuiHtmlPresenter, XuiImage, XuiText, XuiImagePresenter,
XuiTextPresenter, XuiNineGrid, XuiScrollEnd, XuiVideo and XuiCaret.
No unknown serialized class occurs. There is no standalone XuiList record;
XuiList contributes a base-class group to XuiCommonList.

Game-specific behavior appears as **ClassOverride string properties** on
ordinary XUI classes. Observed values are `scene`, `label`, `button`,
`navbutton`, `radiobutton`, `backbutton`, `video`, `radiogroup`, `commonlist`,
`edit`, `htmlcontrol`, `tabscene`, `game_scene`, `slider` and `external_render`.
These differ from Visual skin names. Evidence: InGame.xur scene override at
21a is `scene`, HUD label override at 239 is `external_render`;
menuHud_3.xur's tab scene override at 948 is `tabscene`, its ordinary
controllerSettings navigation override at afe is `navbutton`.
Their registered runtime implementations and press handlers are not in XUR.
A genuinely custom serialized class would require a supplied property schema;
the current parser would show its DATA as raw rather than infer inheritance.

## Named frames and timelines

When element flag bit 2 is set, u32 named-frame count follows its children.
Each frame is `u16 name_index, u32 frame_number, u8 command, u16 target_index`.
`0xffff` is a missing target; index 0 is the empty string. Numeric command
values 0, 1, 2 and 3 occur and are kept without guessing runtime semantics.

If the element has children, u32 timeline count follows the frames; on a leaf
there is **no** timeline-count field. Each timeline is:

1. u16 target Id string index, u32 animated-property-definition count.
2. For each definition: u8 packed depth/indexed flag, u8 class index,
   `depth` u8 property ordinals, and u32 array index if flag bit 7 is set.
   Depth is the low seven bits. Here the class hierarchy is **derived first**,
   the reverse of static property-group order. Compound paths step into
   Stroke/Fill/Gradient schemas.
3. u32 keyframe count.
4. Each keyframe: u32 frame number, four bytes for interpolation/ease-in/
   ease-out/ease-scale, then one wire value per definition. Indexed animated
   values have no repeated array-count byte.

Interpolation byte values 0/1/2 occur. Their curve semantics and command
execution behavior are unresolved; the dump exposes numeric values and raw
property bytes. Timeline target lookup is scoped to the owning subtree.

Evidence: menuHud_3.xur Tab1 named frames start at b11. Its controllerSettings
opacity timeline at b39 has definition at b3f: depth=1, class_index=3,
ordinal=6, meaning XuiElement.Opacity in a NavButton/Button/Control/Element
chain. Its keys at frames 0,40,41,55 carry opacity 0,1,1,0. Both XuiElement
vector properties and deeper indexed gradient paths parse across the skin's
336 timelines. All animation bytes independently re-encode identically.

Navigation can itself be animated. DisplayOptions.xur's brightness-background
keys set NavUp/Down at 95a onward, including a `Preset_button1` reference at
98a which has no matching static element. audio.xur changes voiceOverSlider's
NavDown to speakerChoices at frame 40 (9bc), and speakerChoices.NavDown to
subtitleChoices at d31. Static links alone do not describe every focus state.

## Localization companions

The XUR STRN table contains Ids, classes, visuals, paths and some literal Text
values. Most menu Text properties are absent, not numeric localization IDs.
Visible strings are in matching `.xus` entries in
`game/Local/<locale>/XUIFrontEnd/Scene.xzp`. The English archive has 110
companions for the 110 distinct scenes. It also contains credits HTML and
localized assets. `game/Local/<locale>/GameStrings.str` is a separate store;
its `LSB2` magic was observed, but its layout/use was not decoded in this task.

XUS has magic `XUIS`, bytes `01 00` at 04, u32 total size at 06, u16 record
count at 0a, then records beginning at 0c. Each record is two length-prefixed
UTF-16BE strings, **value followed by dot-separated element/property path**.
The separate meanings of the two marker bytes at 04 are unknown (calling this
“v1” refers to the observed marker, not a proven u16 version encoding).
Empty values are allowed. PointSize as well as Text is localized this way.
The tools keep overrides separate from the base tree rather than silently
replacing literal values or resolving ambiguous Id paths.

Evidence: English menuHud_3.xus is 4,604 bytes with 58 records; the short label
`Options` overrides `Menu.Menu.Tab1.optionsButton.Text` at 566. The `Quit`
label overrides `Menu.Menu.Tab2.quit.Text` at 90c. English DisplayOptions.xus
has its pageTitle Text override at 5a2. Every English companion consumes
exactly its advertised count and size and rebuilds byte-identically. Some
base Text values are image paths (for example controllerOptions Tab1 at
165b); localized empty Text overrides mean base strings cannot be assumed to
be what the player sees.

## Menu scenes

### options.xur and the actual options menu

**No `options.xur` exists** among the 1,013 entries in the twelve requested
packages, including case-insensitive filename checks. No such English locale
companion exists either. The options controls are inside `menuHud_3.xur` at
`XuiCanvas / XuiScene(Menu) / XuiTabScene(Menu) / XuiScene(Tab1)`.
The two Menu Ids are distinct nested nodes; paths matter.

Tab1 at 9e2 has DefaultFocus=controllerSettings and is initially Show=false.
It contains six **XuiNavButtons**, not XuiListItems or an XuiList. The header
button is optionsButton; the five subbuttons form this explicit NavDown cycle:

`controllerSettings -> liveSetup -> audioSetup -> displaySetup -> gameCredits -> controllerSettings`

NavUp follows the reverse cycle. optionsButton at a0c separately points
up to gameCredits and down to controllerSettings. Its visual is mainmenu_left;
subbuttons use `new-subMenuButton2`, override `navbutton`, dimensions 350x37,
and positions with x=259, y=372.6 through 524.6 at approximately 38-unit steps.
Their file targets are:

| Id / element offset | PressPath |
| --- | --- |
| controllerSettings / ae1 | controllerOptions.xur (property at b0b) |
| liveSetup / ab5 | liveSettingPopUp.xur |
| audioSetup / a88 | audio.xur |
| displaySetup / a5b | DisplayOptions.xur |
| gameCredits / a2f | credits.xur |

Tab1 has four transition frame names and one four-key opacity timeline for
each subbutton. English labels come from menuHud_3.xus property paths.

A proposed PC Settings entry should clone a subbutton's XuiNavButton structure,
use a unique Id such as pcSettings, ClassOverride=navbutton, the same visual,
and a PressPath for the new scene. It needs a new STRN Id/path, a VECT position,
space in the layout, a changed Tab1 child count, coherent masks/property counts,
and a matching opacity timeline targeting its Id. Inserting after
controllerSettings requires changing that button's NavDown, liveSetup.NavUp,
and the new node's two links. A sixth row exceeds the current vertical span;
move/compact the rows and check the footer in the renderer. Add localized
Text at `Menu.Menu.Tab1.pcSettings.Text` or establish how a literal Text is
protected from locale overrides. Defaults, focus and press behavior must be
validated in the game before adopting this proposal.

### DisplayOptions.xur

Fourteen elements. Canvas -> scene DisplayOptions at 5ef (override scene,
DefaultFocus=preset), plus a back button, footer scene, background/pageTitle
labels, reset_button (NavButton), brightness-background and HUD-background
(Buttons), brightnessSlider and HUDSlider (Sliders), and preset (RadioGroup)
with XuiRadioButton1/2.

The static NavDown cycle is `preset -> brightnessSlider -> HUDSlider ->
reset_button -> preset`; NavUp reverses it. Reset has no stored PressPath,
so its action cannot be inferred from ordinary navigation. Class overrides
include navbutton, button, slider, radiogroup and label. Four frame names and
eight timelines implement transitions; the button-background navigation
animation noted above must also be considered. Text is in DisplayOptions.xus.

### audio.xur

Twenty elements. Canvas -> scene AudioOptions at 691 (override scene,
DefaultFocus=musicSlider), back button/footer/background/pageTitle,
reset_button and jukebox_button (NavButtons), musicSlider/voiceOverSlider,
audioVol-background/voiceVol-background (Buttons), subtitleChoices and
speakerChoices (RadioGroups), their radio-button pairs and label captions.

The static NavDown cycle is `musicSlider -> voiceOverSlider -> subtitleChoices
-> jukebox_button -> reset_button -> musicSlider`; speakerChoices also links
up to voiceOverSlider and down to jukebox_button. At transition frame 40,
animated links insert speakerChoices before subtitleChoices. jukebox_button
has PressPath=jukebox.xur at 778; reset_button has none. Overrides are scene,
navbutton, button, slider, radiogroup and label. Four frame names and twelve
timelines are decoded. Text is in audio.xus.

### controllerOptions.xur

Eighty-seven elements. Main scene at 134d (override scene,
DefaultFocus=vibration), Background/footer/back/pageTitle, row scenes
vibration (13fd), invert (14a9), layout (1559), and XuiTabScene1 (1609).
Each row owns RadioGroup `options`; children are On/Off, Normal/Inverted,
and southpaw1/default1 respectively. Row-local Ids repeat and need scoped paths.
Labels and figure divider lines belong to two controller diagram tabs.

NavDown on rows cycles vibration -> invert -> layout -> vibration; vibration
NavUp points to XuiTabScene1, while invert/layout point to the preceding row.
The tab scene links up to `layout\options`, down to `vibration\options`.
Overrides include scene, radiogroup, tabscene and label; the class field itself
remains a standard Xui class. Sixteen named frames and 47 timelines include
controller diagram transitions. Text comes from controllerOptions.xus.

### InGame.xur

Seven elements, no animations. Canvas -> scene `In Game` at 205 (override
scene), HUD label at 224 (override external_render), Logo/Paused labels,
Pause Button at 27c and Map Button at 29f (XuiButtons, override button).
Both buttons have Show=false and stored PressKey=0x5814; they have no
PressPath or explicit navigation links. There is no list of pause actions in
this binary. English Paused text is a locale override at XUS offset 226.
Which handler distinguishes the two hidden buttons and opens the menu needs
a runtime trace; the press-key number alone is not proof of the event route.

### menuHud_3.xur

Thirty elements. The main scene at 915 contains the Menu tab scene at 933,
a back button, pause label and Hidden Button. The tab scene contains a footer,
three header placeholders and Tab1/Tab2/Tab3 scenes. Tab1 is the options menu
above. Tab2 at c6a is the main-menu page (DefaultFocus=campaign). All its
items are XuiNavButtons; mainMenuButton is the header. The action cycle is:

`campaign -> timeTrials -> DLC -> extract -> quit -> campaign`

NavUp reverses that cycle. The header links up to quit/down to campaign.
Overrides on subbuttons are navbutton. quit at cb6 is 350x37, Position
approximately (515,524.6,0), Visual=new-subMenuButton2, NavUp=extract,
NavDown=campaign, PressPath=quit.xur at ce0. The named file is **absent** from
all twelve packages. Tab3 contains map, dossier, achievements and leaderboards,
also NavButtons with explicit cyclic links, and agencyInfo as its header.
Some other PressPaths (including map.xur, timeTrials.xur and crackdown.xur)
also do not name literal entries in the package. Do not assume these are dead
links: the override may dispatch, substitute or intercept them at runtime.

The file has twenty named frames and twenty timelines. Every Tab2 subbutton
has its own four-key opacity animation, including quit. The existing Quit
entry could potentially be relabeled and routed to a host exit handler while
keeping its links/animation, but that is a **proposal**, not validated behavior.
If a separate Quit Game action is required, clone this node, provide a unique
Id, repair both neighboring links, fit the extra row, clone its timeline and
add `Menu.Menu.Tab2.<id>.Text` localization. Trace the navbutton override and
existing quit event before choosing whether PressPath or an Id-based handler
is the right interception point. A new XUR item alone cannot terminate the PC
process. Adding it directly under InGame would not match the observed menu
structure.

### save_game_list.xur

Six elements. Canvas -> scene `Save Game List` at 315 (override scene), footer
at 33f, background/pageTitle/waitText labels; waitText has override label.
There are **no serialized list controls, buttons or list items**. Footer
NavUp=JoinPlayerButton and NavDown=inviteFriendButton point to absent static
nodes. Four transition frame names and three timelines animate the labels.
The English pageTitle override at XUS 1fe identifies a storage-device page.
A runtime-populated selector or platform dialog is a hypothesis; DATA does not
prove which. No fabricated save-slot rows are shown by the parser.

## Writer scope and remaining questions

The writer rebuilds STRN, VECT, QUAT, CUST geometry, DATA properties/children,
named frames, animation definitions/keys, section descriptors, file sizes and
extended counts. Tests demonstrate scalar, string-table, vector-table and
keyframe changes survive a serialize/parse cycle. It is not a cached-input
roundtrip. The checked semantic editing layer described below builds on this
writer. Raw bytes are still retained for inspection, NaN payload preservation
and unknown-section/partial-DATA preservation.

String/vector/quaternion/custom references retain **indices**, which are the
writer's authority. Their resolved `value` fields are inspection views; edit
the owning table to change referenced data. Direct scalar property `value`
fields are serialized. Group properties are authoritative; the node's flattened
properties list references those same records. Compound count/mask/marker,
class masks/packed descriptors, flags and element property_count must remain
consistent when adding/removing fields. The writer does not automatically
intern strings, allocate geometry, generate defaults, rebuild masks or clone
scenes. Length-changing CUST edits also require updating later CUST reference
offsets. Source offsets are provenance from the original file, not recomputed
annotations after an edit; reparse serialized bytes to obtain new offsets.

The editing layer below updates these wire relationships. Safe load-time use
still requires game runtime evidence for the scene/navbutton override handlers,
localization application timing, focus/transition behavior and visual layout
checks on the Windows run host. There is no missing wire record preventing
byte-identical roundtrip of the supplied XURs. The absent options/quit filenames,
save selector population, animation command/curve semantics and exact geometry
conventions remain explicit runtime/semantic questions.

## Public references and implementation provenance

The public [XUIHelper project](https://github.com/SGCSam/XUIHelper) describes v5
support and helped locate these concepts. Its
[header](https://github.com/SGCSam/XUIHelper/blob/main/XUIHelper.Core/XUR/V5/XUR5Header.cs),
[DATA format reader](https://github.com/SGCSam/XUIHelper/blob/main/XUIHelper.Core/XUR/V5/Sections/DATA5Section.cs),
[value/animation reader](https://github.com/SGCSam/XUIHelper/blob/main/XUIHelper.Core/XUR/V5/XUR5ReadExtensions.cs)
and [class metadata](https://github.com/SGCSam/XUIHelper/blob/main/XUIHelper.Core/Assets/Extensions/V5/XuiElements.xml)
were consulted as format references. These Python tools independently implement
bounds-checked readers, retained wire records and serialization; no C# code or
XML asset was copied into the repository. Names/types are represented as wire
schema facts. The observations and exact byte comparisons above, not a public
parser's successful conversion, support claims about this game's files.

## Checked semantic editing and PC menu recipe

`tools/xui/edit.py` requires a fully parsed scene. `Editor.find()` walks exact,
scoped Id paths, omitting an anonymous canvas and optionally including a named
root. It refuses missing or ambiguous siblings. `clone(source, parent, new_id,
child_index)` deep-copies a subtree within the same scene, inserts at a checked
child index, and retargets self references and subtree-owned animations.
Descendant Ids remain unchanged in their own scope. Ancestor-owned animations
are explicit: `clone_timelines(owner, source, clone)` copies their definitions,
keys and interpolation bytes and updates the target string index. It refuses
ambiguous targets and different class hierarchies.

`set_property()` adds or replaces a schema-defined property. It interns strings
and vectors/quaternions without modifying shared entries, creates a missing
vector/quaternion section when necessary, and rebuilds the base-first groups,
packed descriptors, masks, flat references, property count and property flag.
Scalar, array and compound fields are supported. CUST values must reference an
existing scene record; allocating/relocating new geometry is outside this API.
Section offsets/sizes, child/animation counts and extended header counts are
rebuilt on serialization. `finish()` strictly reparses the entire output and
requires a lossless serialization of that parse. `set_localized()` adds or
replaces one XUS property path, refuses duplicate overrides, and verifies the
updated count, size and complete parse.

Run the recipe and tests from the repository root:

```sh
python3 -m unittest discover -s tools/xui -v
python3 -m tools.xui.mod_menus ~/DecompProject/game
```

The recipe reads `git show wip/launcher:tools/game-manifest.txt` (or the explicit
`--manifest` file used in [pc-options.md](pc-options.md)) and verifies
**every input package before producing any output**. A missing manifest row,
missing SHA256 (`-`), wrong size/hash, or input symlink outside the game root
fails closed. English is mandatory; other existing `Local/*/XUIFrontEnd/Scene.xzp`
packages are discovered automatically and require their own manifest hash.
The manifest has SHA256 values for all 15 recipe packages of this reference disc,
including the standalone menuHUD_3.xzp used at runtime.
`--manifest PATH` can explicitly select another trusted manifest in the same
`path<TAB>size<TAB>sha256` format. This checks all recipe inputs, not unrelated
game files; full-disc verification remains the launcher's responsibility.

Generated artifacts are confined to the ignored `out/xui/` tree. Writes use
a temporary sibling and atomic replacement so an existing output hardlinked
to an original cannot truncate the original:


- `modified/game/XUIFrontEnd/menuHUD_3.xzp` and its `patches/XUIFrontEnd/menuHUD_3.xzp.xpatch`.
- `modified/game/XUIFrontEnd/Scene.xzp` and `modified/game/Local/<locale>/XUIFrontEnd/Scene.xzp`.
- `patches/XUIFrontEnd/Scene.xzp.xpatch` and `patches/Local/<locale>/XUIFrontEnd/Scene.xzp.xpatch`.
- `patches/report.json`, containing input/result SHA256, package validation counts,
  patch sizes, literal-byte totals and the original-data audit.

Packages, companions and inspection dumps remain local under `out/`. The audited
standalone runtime patch is committed as `patches/xui/menuHUD_3.xpatch`; the
other generated patches remain local. See [pc-options.md](pc-options.md).

The base package changes `menuHud_3.xur` at
`Menu/Menu/Tab1/displaySetup`: its deep clone `pcSettings` is inserted at the
next child index, has an empty PressPath for milestone-1 host routing, and
receives an identical opacity timeline. The focus cycle becomes:

```text
controllerSettings -> liveSetup -> audioSetup -> displaySetup
                   -> pcSettings -> gameCredits -> controllerSettings
```

NavUp reverses that cycle. The six controls have height/pitch 31.5, x=259,
y=372.6, 404.1, 435.6, 467.1, 498.6, 530.1. This fills exactly the original
189-unit footprint (five 37-unit rows at 38-unit pitch), ending at y=561.6.
The options header and footer are preserved. Runtime skin scaling and text
fit still need a visual check. Tab2 also gains `quitGame` after `quit` in its
cycle, an empty PressPath, literal Text "Quit to Desktop", a cloned opacity
timeline, and the same row fit at x=515. The existing quit retains
`PressPath=quit.xur`; its navigation and position change to fit the new row.
Tab2 mainMenuButton.NavUp points to quitGame.

`pcsettings.xur` is derived from `DisplayOptions.xur`. The root scene Id is
`PCSettings`, its default focus is `framerate`, and its self-navigation links
use the new Id. It retains the original canvas, background, pageTitle, footer,
back button and chrome transitions. It contains:

| Scoped Id below PCSettings | Class / placeholder |
| --- | --- |
| framerate | RadioGroup cloned from preset; XuiRadioButton1 = 60 fps, XuiRadioButton2 = 30 fps |
| resolution | Second preset clone; XuiRadioButton1 = 720p, XuiRadioButton2 = 1440p |
| mousesens | Slider cloned from HUDSlider; range 1–100, step 1, initial value 50 |
| mousesensLabel | Label cloned from pageTitle; Mouse sensitivity |
| quitgame | NavButton cloned from reset_button; Quit to desktop |

The row cycle is `framerate -> resolution -> mousesens -> quitgame -> framerate`,
with reverse NavUp. Each radio pair preserves its left/right links; up/down
explicitly follow the row cycle. Control opacity animations are cloned.
The caption inherits only title opacity, avoiding its animated title position.
Old preset/brightness/HUD/reset controls and their animations are removed,
including the inherited animated references to the absent `Preset_button1`.
`quitgame` has no PressPath and awaits a host action handler. These controls do
not yet change PC settings or exit the process.

Every supplied locale gets `Menu.Menu.Tab1.pcSettings.Text = PC Settings`,
`Menu.Menu.Tab2.quitGame.Text = Quit to Desktop`, and a new `pcsettings.xus`
derived from its own `DisplayOptions.xus`. Surviving
chrome overrides and control PointSize values are retained/remapped. All new
labels are **English for every locale**, including Japanese, Korean and Chinese.
The base XUR also contains the new literal labels. Localization application
order and font/skin rendering need a Windows run.

## Patch format

XUIPAT1 is an uncompressed, dependency-free binary patch. All integers are
unsigned and **big-endian**. COPY may reference any original offset repeatedly
and out of order; there is no reference to already-produced output. A patch
is bound to one exact original and one exact result by size and SHA256.

| Header offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 8 | Magic/version `58 55 49 50 41 54 31 00` (`XUIPAT1\0`) |
| 8 | 8 | Original size, u64 |
| 16 | 8 | Result size, u64 |
| 24 | 32 | Original SHA256, raw digest bytes |
| 56 | 32 | Result SHA256, raw digest bytes |
| 88 | 4 | Operation count, u32 |
| 92 | variable | Exactly that many operations; no trailer |

Each operation is one of:

- **COPY**: opcode `00`, original offset u64, length u64 (17 bytes total).
- **INSERT**: opcode `01`, length u64, then exactly `length` literal bytes
  (9 bytes overhead plus literal bytes).

Lengths must be nonzero. Zero operations are valid for an empty result.
Reject unknown magic/opcodes, truncated fields/payloads, extra bytes, COPY
ranges outside the original, operation lengths exceeding remaining result
space, and a final size different from the advertised result size. In C++,
check `offset <= original_size` and `length <= original_size - offset` instead
of unchecked addition; likewise compare length to remaining output capacity.
Verify original size/hash **before using its bytes**. Only expose or atomically
publish the result after its size/hash also pass. SHA256 is an integrity
binding, not patch publisher authentication; the launcher must obtain patches
from its trusted release.

`tools/xui/xpatch.py:apply(original, patch)` and its strict `decode()` reader
need no XUI parser and use only Python stdlib (hashlib/struct). They return
verified result bytes. A C++ port needs the same bounds checks, concatenation
and SHA256 routines; no compression or diff library is involved at runtime.

```sh
python3 -m tools.xui.xpatch generate ORIGINAL MODIFIED out/xui/example.xpatch
python3 -m tools.xui.xpatch audit ORIGINAL out/xui/example.xpatch
python3 -m tools.xui.xpatch apply ORIGINAL out/xui/example.xpatch out/xui/applied.xzp
```

The generator first recognizes XUIZ packages. It diffs the header and individual
small directory records, takes unchanged payload entries directly as whole
COPY runs, and uses `difflib.SequenceMatcher(autojunk=False)` within changed
entries. New pcsettings entries are diffed against the original DisplayOptions
companions via source aliases supplied by the recipe. Adjacent compatible
operations are coalesced. Arbitrary binary files instead use a 16-byte block
index and target scanning.

A second, exhaustive pass finds every original-matching **16-byte window**
inside INSERT data (including unaligned offsets and other package entries),
converts its longest matching span into COPY, then coalesces again. The audit
independently counts matching windows, total literal bytes and longest INSERT.
The recipe requires **zero original-matching literal windows of length 16**.
Consequently no long INSERT contains even a 16-byte contiguous copy of original
game data. Short incidental common fragments can remain as literal metadata;
the audit uses this explicit threshold. Game assets and cloned scene/chrome content are reconstructed from COPY operations,
not embedded as long literals. Patch size includes header/opcode overhead and
can exceed the literal total substantially.

Every recipe result is checked by full XZP reconstruction, full strict parse and
lossless reconstruction of every XUR/XUS, plus byte equality of
`apply(original, patch)` with the modified package. Only the patched PC copy
should be redirected into the open path. The game files themselves are never
changed. This milestone redirects only the standalone runtime menu package.
A future native pcsettings page would also require the base/selected-locale
archives; the reported cache/local/base lookup order alone does not establish
localization overlay behavior.

Wire validity does not prove runtime acceptance. The main scene grows from
30 to 32 elements (20 to 22 timelines); the new page has 15 elements versus
DisplayOptions' 14 and seven timelines. Unknown risks include hard-coded
per-page counts, Id-based game callbacks (the old names have been removed),
scene-filename or Id dispatch/registration despite standard serialized classes,
focus behavior inside RadioGroups, skin height/font constraints, transition
commands and localization ordering. All class/visual names are existing ones;
`PCSettings` is a new scene Id, not a new serialized class. The milestone-1
open redirection is enabled by default behind
`--pc_menu_mod`; Windows runtime load/navigation/exit evidence is still required
before merge. The existing main-menu quit action retains its original callback.
See [pc-options.md](pc-options.md) for the full pipeline and remaining assumptions.

## Launcher art reader

The separate C++ reader and verified art cache are documented in
[launcher-xzp.md](launcher-xzp.md). They use the same XUIZ package format.
