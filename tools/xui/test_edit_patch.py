"""Synthetic semantic edit, patch integrity, and recipe tests (no game content)."""
from copy import deepcopy
import hashlib
import os
from pathlib import Path
import random
import struct
import tempfile
import unittest

from tools.xui import edit, mod_menus, xpatch, xur, xus, xzp
from tools.xui.test_xui import basic_scene, scene_blob


def empty_editor():
    return edit.Editor.parse(scene_blob(['XuiCanvas'], b'\x00\x01\x00'))


class GuestMaskWidthTests(unittest.TestCase):
    def test_high_property_masks_use_four_bytes(self):
        from unittest.mock import patch
        from tools.xui.schema import CLASSES
        editor = empty_editor()
        obj = editor.scene.root
        # A synthetic newer class exercises the loader's width constraint
        # without pretending this game's Element supports newer fields.
        obj['class'] = 'XuiMaskFixture'
        obj['class_index'] = editor.intern(obj['class'])
        fields = [(f'Flag{i}', 'b') for i in range(19)]
        with patch.dict(CLASSES, {'XuiMaskFixture': (None, fields)}):
            editor.set_property(obj, 'Flag18', False)
            group = obj['property_groups'][0]
            self.assertEqual(group['packed'] & 7, 4)
            self.assertEqual(len(bytes.fromhex(group['mask'])), 4)
            parsed = xur.parse(editor.finish())
            self.assertTrue(parsed.fully_parsed)
            self.assertFalse(edit.property_value(parsed.root, 'Flag18'))

    def test_three_byte_mask_is_rejected(self):
        # A syntactically decodable mask that the guest loader cannot read.
        data = scene_blob(['XuiCanvas'], b'\x00\x01\x01\x00\x01\x03\x04\x00\x00\x00\x00')
        parsed = xur.parse(data)
        self.assertFalse(parsed.fully_parsed)
        self.assertTrue(any('unsupported property mask width' in e for e in parsed.errors))


def add(editor, parent, cls, ident, **props):
    obj = dict(class_index=editor.intern(cls), **{'class': cls}, id='', flags=0,
               properties=[], property_groups=[], children=[], named_frames=[], timelines=[])
    editor.set_property(obj, 'Id', ident)
    for name, value in props.items():
        editor.set_property(obj, name, value)
    parent['children'].append(obj)
    parent['flags'] |= 2
    return obj


def opacity(editor, owner, target, position=False):
    from tools.xui.schema import hierarchy
    chain = hierarchy(target['class'])
    definitions = [dict(packed=1, class_index=chain.index('XuiElement'), path=[6], name='Opacity', type='f', array_index=None)]
    if position:
        definitions.append(dict(packed=1, class_index=chain.index('XuiElement'), path=[3], name='Position', type='v', array_index=None))
    keys = []
    for frame, value in [(0, 0.0), (40, 1.0)]:
        props = [editor.make_property('Opacity', 'f', value)]
        if position:
            props.append(editor.make_property('Position', 'v', [0.0, 10.0, 0.0]))
        keys.append(dict(frame=frame, interpolation=[0, 0, 0, 50], properties=props))
    owner['timelines'].append(dict(target=target['id'], target_index=editor.intern(target['id']), definitions=definitions, keyframes=keys))
    owner['flags'] |= 4


def menu_fixture():
    editor = empty_editor()
    menu = add(editor, editor.scene.root, 'XuiScene', 'Menu')
    tabs = add(editor, menu, 'XuiTabScene', 'Menu')
    tab = add(editor, tabs, 'XuiScene', 'Tab1')
    for ident in ['optionsButton', 'gameCredits', 'displaySetup', 'audioSetup', 'liveSetup', 'controllerSettings']:
        obj = add(editor, tab, 'XuiNavButton', ident, Height=37.0, Position=[259.0, 0.0, 0.0], PressPath='Synthetic.xur')
        if ident != 'optionsButton':
            opacity(editor, tab, obj)
    tab2 = add(editor, tabs, 'XuiScene', 'Tab2')
    add(editor, tab2, 'XuiNavButton', 'mainMenuButton', NavUp='quit', NavDown='campaign')
    for ident in ['quit', 'extract', 'DLC', 'timeTrials', 'campaign']:
        obj = add(editor, tab2, 'XuiNavButton', ident, Height=37.0, Position=[515.0, 0.0, 0.0], PressPath=ident+'.xur')
        opacity(editor, tab2, obj)
    return editor.finish()


def display_fixture():
    editor = empty_editor()
    page = add(editor, editor.scene.root, 'XuiScene', 'DisplayOptions', DefaultFocus='preset')
    for ident, cls in [('XuiBackButton1', 'XuiBackButton'), ('footer', 'XuiScene'),
                       ('background2', 'XuiLabel'), ('pageTitle', 'XuiLabel'),
                       ('preset', 'XuiRadioGroup'), ('HUDSlider', 'XuiSlider'),
                       ('brightnessSlider', 'XuiSlider'), ('reset_button', 'XuiNavButton'),
                       ('brightness-background', 'XuiButton'), ('HUD-background', 'XuiButton')]:
        obj = add(editor, page, cls, ident, Opacity=0.0)
        if ident not in ('XuiBackButton1', 'footer'):
            opacity(editor, page, obj, position=(ident == 'pageTitle'))
        if ident == 'preset':
            add(editor, obj, 'XuiRadioButton', 'XuiRadioButton1', NavLeft='XuiRadioButton2', NavRight='XuiRadioButton2')
            add(editor, obj, 'XuiRadioButton', 'XuiRadioButton2', NavLeft='XuiRadioButton1', NavRight='XuiRadioButton1')
    return editor.finish()


class EditTests(unittest.TestCase):
    def test_add_replace_properties_masks_tables_counts(self):
        editor = edit.Editor.parse(basic_scene())
        node = editor.scene.root
        original_position = deepcopy(editor.scene.vectors)
        editor.set_property(node, 'Text', 'Different \U0001f600')
        editor.set_property(node, 'Show', False)
        editor.set_property(node, 'Position', [88.0, 34.0, 0.0])
        editor.set_property(node, 'ColorWriteFlags', 15)
        editor.set_property(node, 'Width', 640.0)
        rebuilt = xur.parse(editor.finish())
        props = {p['name']:p for p in rebuilt.root['properties']}
        self.assertFalse(props['Show']['value'])
        self.assertEqual(props['Text']['value'], 'Different \U0001f600')
        self.assertEqual(props['Position']['value'], [88, 34, 0])
        self.assertEqual(rebuilt.root['property_count'], 7)
        self.assertEqual(rebuilt.root['property_groups'][0]['mask'], '220b')
        self.assertEqual(editor.scene.vectors[0], original_position[0])
        self.assertEqual(editor.scene.strings[3]['value'], 'Synthetic')
        with self.assertRaises(ValueError):
            editor.set_property(node, 'PressPath', 'bad.xur')
        for unsupported in ['ColorFactor', 'Hittable', 'DisableFocusRecursion', 'NavTabForward']:
            with self.assertRaises(ValueError):
                editor.set_property(node, unsupported, False)
        with self.assertRaises(ValueError):
            editor.set_property(node, 'Show', 1)

    def test_scope_clone_deep_child_index_and_new_parent(self):
        editor = empty_editor()
        left = add(editor, editor.scene.root, 'XuiScene', 'Menu')
        inner = add(editor, left, 'XuiScene', 'Menu')
        source = add(editor, inner, 'XuiNavButton', 'item', Text='Synthetic', Position=[1, 2, 3])
        child = add(editor, source, 'XuiLabel', 'caption', Text='Nested')
        right = add(editor, editor.scene.root, 'XuiScene', 'Other')
        self.assertIs(editor.find('Menu/Menu/item/caption'), child)
        clone = editor.clone(source, inner, 'copy', 0)
        self.assertIs(inner['children'][0], clone)
        editor.set_property(clone['children'][0], 'Text', 'Changed')
        self.assertEqual(edit.property_value(child, 'Text'), 'Nested')
        moved = editor.clone(source, right, 'elsewhere', 0)
        editor.set_property(moved, 'Position', [9, 8, 7])
        self.assertEqual(edit.property_value(source, 'Position'), [1, 2, 3])
        rebuilt = xur.parse(editor.finish())
        self.assertEqual(edit.find(rebuilt.root, 'Other/elsewhere')['id'], 'elsewhere')
        for path in ['item', 'Menu/item', 'Menu//Menu', 'Missing']:
            with self.assertRaises(ValueError):
                editor.find(path)
        with self.assertRaises(ValueError):
            editor.clone(source, inner, 'copy')
        with self.assertRaises(ValueError):
            editor.clone(source, inner, 'bad', 100)
        inner['children'].append(deepcopy(source))
        with self.assertRaises(ValueError):
            editor.find('Menu/Menu/item')

    def test_animation_and_extended_header_counts(self):
        editor = empty_editor()
        editor.scene.flags = 1
        editor.scene.count_header = [0]*10
        owner = add(editor, editor.scene.root, 'XuiScene', 'Page')
        source = add(editor, owner, 'XuiNavButton', 'original')
        opacity(editor, owner, source)
        clone = editor.clone(source, owner, 'copy')
        animations = editor.clone_timelines(owner, source, clone)
        animations[0]['keyframes'][1]['properties'][0]['value'] = 0.5
        self.assertEqual(owner['timelines'][0]['keyframes'][1]['properties'][0]['value'], 1.0)
        parsed = xur.parse(editor.finish())
        self.assertEqual(parsed.count_header, parsed.computed_counts())
        self.assertEqual(parsed.count_header[0], 4)
        self.assertEqual(parsed.count_header[7], 2)
        page = edit.find(parsed.root, 'Page')
        self.assertEqual([t['target'] for t in page['timelines']], ['original', 'copy'])
        self.assertEqual(page['timelines'][1]['keyframes'][1]['properties'][0]['value'], 0.5)

    def test_clone_owned_animation_and_self_reference(self):
        editor = empty_editor()
        group = add(editor, editor.scene.root, 'XuiScene', 'Group', NavUp='Group')
        child = add(editor, group, 'XuiButton', 'Child')
        opacity(editor, group, child)
        clone = editor.clone(group, editor.scene.root, 'GroupCopy')
        self.assertEqual(edit.property_value(clone, 'NavUp'), 'GroupCopy')
        self.assertEqual(clone['timelines'][0]['target'], 'Child')
        self.assertTrue(xur.parse(editor.finish()).fully_parsed)

    def test_clone_retargets_animated_self_links_but_preserves_text(self):
        editor = empty_editor()
        page = add(editor, editor.scene.root, 'XuiScene', 'Page')
        source = add(editor, page, 'XuiNavButton', 'original', Text='original', NavUp='original')
        opacity(editor, page, source)
        timeline = page['timelines'][0]
        timeline['definitions'].append(dict(packed=1, class_index=2, path=[6], name='NavUp', type='s', array_index=None))
        for key in timeline['keyframes']:
            key['properties'].append(editor.make_property('NavUp', 's', 'original'))
        clone = editor.clone(source, page, 'copy')
        editor.clone_timelines(page, source, clone)
        self.assertEqual(edit.property_value(clone, 'Text'), 'original')
        self.assertEqual(edit.property_value(clone, 'NavUp'), 'copy')
        rebuilt = xur.parse(editor.finish())
        timeline = edit.find(rebuilt.root, 'Page')['timelines'][1]
        self.assertEqual(timeline['keyframes'][1]['properties'][1]['value'], 'copy')
        self.assertEqual(page['timelines'][0]['keyframes'][1]['properties'][1]['value'], 'original')
        impostor = deepcopy(source)
        with self.assertRaises(ValueError):
            editor.clone_timelines(page, impostor, clone)

    def test_missing_vector_section_and_compound_arrays(self):
        editor = empty_editor()
        figure = add(editor, editor.scene.root, 'XuiFigure', 'Fig')
        editor.set_property(figure, 'Position', [1.0, 2.0, 3.0])
        value = dict(FillType=1, Gradient=dict(NumStops=2, StopColor=['#ff000000', '#ffffffff'], StopPos=[0.0, 1.0]))
        editor.set_property(figure, 'Fill', value)
        parsed = xur.parse(editor.finish())
        self.assertTrue(parsed.section('VECT'))
        self.assertEqual(edit.property_value(edit.find(parsed.root, 'Fig'), 'Fill'), value)
        with self.assertRaises(ValueError):
            edit.Editor(xur.parse(scene_blob(['UnknownClass'], b'\x00\x01\x01\x00\x00')))

    def test_localized_add_replace_and_duplicate_refusal(self):
        records = xus.parse(xus.build([dict(path='Page.Title.Text', value='Old')]))
        edit.set_localized(records, 'Page.Title.Text', 'New')
        data = edit.set_localized(records, 'Page.Button.Text', 'Synthetic \U0001f600')
        parsed = xus.parse(data)
        self.assertEqual(len(parsed), 2)
        self.assertEqual(parsed[0]['value'], 'New')
        self.assertEqual(parsed[1]['value'], 'Synthetic \U0001f600')
        records.append(dict(path='Page.Title.Text', value='Duplicate'))
        with self.assertRaises(ValueError):
            edit.set_localized(records, 'Page.Title.Text', 'bad')


class PatchTests(unittest.TestCase):
    def test_general_roundtrip_repeated_moved_and_empty(self):
        rng = random.Random(17)
        original = rng.randbytes(5000)
        results = [original, b'', b'New synthetic bytes', original[400:1400]+b'new'+original[:400]+original[400:1400]]
        for result in results:
            patch = xpatch.generate(original, result)
            self.assertEqual(xpatch.apply(original, patch), result)
            self.assertEqual(xpatch.audit(original, patch)['original_matching_literal_windows'], 0)
        self.assertEqual(xpatch.apply(b'', xpatch.generate(b'', b'')), b'')

    def test_package_whole_entry_copy_and_new_entry_source(self):
        unchanged = bytes(range(256))*20
        scene = b'Synthetic menu string'*20
        original = xzp.build([('asset', unchanged), ('source', scene)])
        result = xzp.build([('asset', unchanged), ('source', scene), ('new', scene[:90]+b'New label'+scene[90:])])
        patch = xpatch.generate(original, result, {'new':'source'})
        self.assertEqual(xpatch.apply(original, patch), result)
        _, ops = xpatch.decode(patch)
        start = 22+struct.unpack_from('>I', original, 16)[0]
        self.assertTrue(any(op[0] == xpatch.COPY and op[1] <= start and op[1]+op[2] >= start+len(unchanged) for op in ops))
        stats = xpatch.audit(original, patch)
        self.assertLess(stats['literal_bytes'], 100)
        self.assertEqual(stats['original_matching_literal_windows'], 0)

    def test_refuse_wrong_original_corruption_truncation_and_trailer(self):
        original, result = b'original synthetic bytes', b'new synthetic content'
        patch = xpatch.generate(original, result)
        with self.assertRaisesRegex(ValueError, 'original'):
            xpatch.apply(b'wrong', patch)
        for n in range(len(patch)):
            with self.subTest(n=n), self.assertRaises(ValueError):
                xpatch.apply(original, patch[:n])
        with self.assertRaises(ValueError):
            xpatch.apply(original, patch+b'extra')
        damaged = bytearray(patch)
        damaged[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'result'):
            xpatch.apply(original, damaged)
        bad = bytearray(patch)
        bad[xpatch.HEADER.size] = 9
        with self.assertRaisesRegex(ValueError, 'opcode'):
            xpatch.apply(original, bad)

    def test_copy_bounds_zero_overflow_and_literal_audit(self):
        original = b'original synthetic asset '*20
        def manual(body, result, count=1):
            return xpatch.HEADER.pack(xpatch.MAGIC, len(original), len(result), xpatch.digest(original), xpatch.digest(result), count)+body
        for offset, length in [(len(original), 1), (2**64-1, 1), (0, 0), (0, len(original)+1)]:
            with self.assertRaises(ValueError):
                xpatch.apply(original, manual(struct.pack('>BQQ', 0, offset, length), b'a'))
        patch = manual(struct.pack('>BQ', 1, len(original))+original, original)
        self.assertGreater(xpatch.audit(original, patch)['original_matching_literal_windows'], 0)
        repaired = xpatch.generate(original, original)
        self.assertEqual(xpatch.audit(original, repaired)['literal_bytes'], 0)


class RecipeTests(unittest.TestCase):
    def test_menu_nav_positions_timeline_and_tab2(self):
        original = menu_fixture()
        data = mod_menus.modify_menu(original)
        parsed = xur.parse(data)
        tab = edit.find(parsed.root, 'Menu/Menu/Tab1')
        clone = edit.find(parsed.root, 'Menu/Menu/Tab1/pcSettings')
        self.assertEqual(clone['class'], 'XuiNavButton')
        self.assertEqual(edit.property_value(clone, 'Text'), 'PC Settings')
        self.assertEqual(edit.property_value(clone, 'PressPath'), '')
        self.assertEqual(edit.property_value(clone, 'NavUp'), 'displaySetup')
        self.assertEqual(edit.property_value(clone, 'NavDown'), 'gameCredits')
        self.assertEqual(edit.property_value(edit.find(parsed.root, 'Menu/Menu/Tab1/displaySetup'), 'NavDown'), 'pcSettings')
        self.assertEqual(edit.property_value(edit.find(parsed.root, 'Menu/Menu/Tab1/gameCredits'), 'NavUp'), 'pcSettings')
        source_index = next(i for i, c in enumerate(tab['children']) if c['id'] == 'displaySetup')
        self.assertIs(tab['children'][source_index+1], clone)
        self.assertEqual(len(tab['timelines']), 6)
        self.assertEqual([t['target'] for t in tab['timelines']].count('pcSettings'), 1)
        desktop = edit.find(parsed.root, 'Menu/Menu/Tab2/quitGame')
        self.assertEqual(edit.property_value(desktop, 'Text'), 'Quit to Desktop')
        self.assertEqual(edit.property_value(desktop, 'PressPath'), '')
        controls2 = [edit.find(parsed.root, 'Menu/Menu/Tab2/'+name) for name in
                     ['campaign', 'timeTrials', 'DLC', 'extract', 'quit', 'quitGame']]
        for i, obj in enumerate(controls2):
            self.assertEqual(edit.property_value(obj, 'NavUp'), controls2[i-1]['id'])
            self.assertEqual(edit.property_value(obj, 'NavDown'), controls2[(i+1)%6]['id'])
            self.assertAlmostEqual(edit.property_value(obj, 'Position')[1], 372.6+31.5*i, places=3)
        self.assertEqual(edit.property_value(controls2[-2], 'PressPath'), 'quit.xur')
        tab2 = edit.find(parsed.root, 'Menu/Menu/Tab2')
        self.assertEqual(len(tab2['timelines']), 6)
        self.assertEqual([t['target'] for t in tab2['timelines']].count('quitGame'), 1)
        original_quit = next(t for t in tab2['timelines'] if t['target'] == 'quit')
        copied_quit = next(t for t in tab2['timelines'] if t['target'] == 'quitGame')
        def animation_values(timeline):
            definitions = [{k:v for k,v in d.items() if k != 'offset'} for d in timeline['definitions']]
            keys = [(k['frame'], k['interpolation'], [(p['name'], p['type'], p['value'])
                     for p in k['properties']]) for k in timeline['keyframes']]
            return definitions, keys
        self.assertEqual(animation_values(original_quit), animation_values(copied_quit))
        self.assertEqual(edit.property_value(edit.find(parsed.root, 'Menu/Menu/Tab2/mainMenuButton'), 'NavUp'), 'quitGame')
        before3 = xur.parse(original)
        # Existing menu controls retain their actions; the desktop clone alone has no link.
        self.assertEqual(edit.property_value(edit.find(before3.root, 'Menu/Menu/Tab2/quit'), 'PressPath'),
                         edit.property_value(controls2[-2], 'PressPath'))
        controls = [c for c in tab['children'] if c['id'] != 'optionsButton']
        self.assertAlmostEqual(max(edit.property_value(c, 'Position')[1]+edit.property_value(c, 'Height') for c in controls), 561.6, places=3)

    def test_standalone_keeps_assets_and_adds_no_page(self):
        original = xzp.build([('menuHud_3.xur', menu_fixture()), ('asset.png', b'Synthetic asset')])
        result = mod_menus.modify_package(original, standalone=True)
        entries = xzp.parse(result)
        self.assertEqual([e.name for e in entries], ['menuHud_3.xur', 'asset.png'])
        self.assertEqual(entries[1].data, b'Synthetic asset')
        patch = xpatch.generate(original, result)
        self.assertEqual(xpatch.apply(original, patch), result)
        self.assertEqual(xpatch.audit(original, patch)['original_matching_literal_windows'], 0)
        with self.assertRaises(ValueError):
            mod_menus.modify_package(original)
        with self.assertRaises(ValueError):
            mod_menus.modify_package(xzp.build([('asset.png', b'Synthetic')]), standalone=True)

    def test_scene_chrome_navigation_and_localizations(self):
        data = mod_menus.pc_scene(display_fixture())
        page = edit.find(xur.parse(data).root, 'options')
        ids = {c['id'] for c in page['children']}
        self.assertTrue({'footer', 'XuiBackButton1', 'background2', 'pageTitle',
                         'AgentProfiles', 'AudioSetup', 'restartNotice'} <= ids)
        self.assertFalse({'preset', 'HUDSlider', 'brightness-background', 'quitgame', 'mousesens'} & ids)
        self.assertEqual(edit.property_value(page, 'ClassOverride'), 'scene')
        self.assertEqual(edit.property_value(page, 'DefaultFocus'), 'framerate')
        self.assertEqual(edit.property_value(edit.find(page, 'pageTitle'), 'Visual'), 'XuiLabel')
        controls = [edit.find(page, name) for name in ['framerate', 'resolution', 'display']]
        for i, group in enumerate(controls):
            self.assertEqual(group['class'], 'XuiRadioGroup')
            for obj in [group]+group['children']:
                self.assertEqual(edit.property_value(obj, 'NavDown'), controls[(i+1)%3]['id'])
                self.assertEqual(edit.property_value(obj, 'NavUp'), controls[i-1]['id'])
            self.assertEqual(len(group['children']), 2)
            for j, button in enumerate(group['children']):
                self.assertEqual(edit.property_value(button, 'NavLeft'), group['children'][1-j]['id'])
                self.assertEqual(edit.property_value(button, 'NavRight'), group['children'][1-j]['id'])
        self.assertEqual(edit.property_value(controls[0]['children'][1], 'Text'), '30 fps (original)')
        self.assertEqual(edit.property_value(controls[2]['children'][1], 'Text'), 'Windowed')
        for ident in ['framerateLabel', 'resolutionLabel', 'displayLabel', 'restartNotice']:
            caption = edit.find(page, ident)
            self.assertEqual(edit.property_value(caption, 'Visual'), 'XuiLabel')
            self.assertFalse(edit.property_value(caption, 'Enabled'))
            self.assertFalse(edit.property_value(caption, 'UnfocusedInput'))
            self.assertFalse({'Hittable', 'DisableFocusRecursion'} & {p['name'] for p in caption['properties']})
        for ident, cls in [('AgentProfiles', 'XuiLabel'), ('AudioSetup', 'XuiNavButton')]:
            obj = edit.find(page, ident)
            self.assertEqual(obj['class'], cls)
            for prop in ['Show', 'Enabled', 'UnfocusedInput']:
                self.assertFalse(edit.property_value(obj, prop))
            for prop in ['NavLeft', 'NavRight', 'NavUp', 'NavDown']:
                self.assertEqual(edit.property_value(obj, prop), '')
            self.assertFalse({'Hittable', 'DisableFocusRecursion'} & {p['name'] for p in obj['properties']})
            self.assertFalse(any(t['target'] == ident for t in page['timelines']))
        self.assertFalse(edit.property_value(edit.find(page, 'footer'), 'Show'))
        self.assertEqual(edit.property_value(edit.find(page, 'XuiBackButton1'), 'Text'), 'Back')
        self.assertTrue(all(t['target'] in ids for t in page['timelines']))
        for t in page['timelines']:
            if t['target'] not in ('pageTitle', 'background2'):
                self.assertEqual([d['name'] for d in t['definitions']], ['Opacity'])
        local = xus.build([dict(path='DisplayOptions.pageTitle.Text', value='Synthetic title'),
                           dict(path='DisplayOptions.footer.Text', value='Synthetic locale footer'),
                           dict(path='DisplayOptions.HUD-background.Text', value='Removed')])
        labels = {r['path']: r['value'] for r in xus.parse(mod_menus.pc_localization(local))}
        self.assertEqual(labels['options.pageTitle.Text'], 'PC Settings')
        self.assertEqual(labels['options.display.XuiRadioButton2.Text'], 'Windowed')
        self.assertNotIn('options.footer.Text', labels)
        for path in labels:
            edit.find(page, '/'.join(path.split('.')[:-1]))

    def test_output_guards_and_hardlink_safe_publish(self):
        root = Path(__file__).resolve().parents[2]/'out/xui'
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as tmp:
            original, target = Path(tmp)/'original', Path(tmp)/'target'
            original.write_bytes(b'Synthetic original')
            os.link(original, target)
            xzp.write_output(target, b'Synthetic modified')
            self.assertEqual(original.read_bytes(), b'Synthetic original')
            self.assertEqual(target.read_bytes(), b'Synthetic modified')
            self.assertNotEqual(original.stat().st_ino, target.stat().st_ino)
            (Path(tmp)/'escape').symlink_to('/tmp')
            with self.assertRaises(ValueError):
                xzp.write_output(Path(tmp)/'escape'/'forbidden', b'Synthetic')
        with self.assertRaises(ValueError):
            xzp.write_output('/tmp/forbidden', b'Synthetic')

    def test_package_recipe_and_hash_gate(self):
        original = xzp.build([('menuHud_3.xur',menu_fixture()), ('DisplayOptions.xur',display_fixture())])
        result = mod_menus.modify_package(original)
        self.assertEqual(mod_menus.validate_package(result)['scenes'], 3)
        entries = {e.name: e.data for e in xzp.parse(result)}
        self.assertEqual(entries['menuHud_3.xur'], menu_fixture())
        self.assertEqual(entries['DisplayOptions.xur'], display_fixture())
        edit.find(xur.parse(entries['options.xur']).root, 'options')
        self.assertEqual(xpatch.apply(original, xpatch.generate(original, result, {'options.xur':'DisplayOptions.xur'})), result)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inputs = ['XUIFrontEnd/menuHUD_3.xzp', 'XUIFrontEnd/Scene.xzp', 'Local/en-US/XUIFrontEnd/Scene.xzp', 'Local/xx-YY/XUIFrontEnd/Scene.xzp']
            for rel in inputs:
                target = root/rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(original)
            text = '\n'.join(f'{rel}\t{len(original)}\t{hashlib.sha256(original).hexdigest()}' for rel in inputs)
            records = mod_menus.manifest_records(text)
            self.assertEqual(len(mod_menus.verify_inputs(root, records)), 4)
            for rel in inputs:
                wrong = deepcopy(records)
                wrong[rel] = (len(original), '-')
                with self.assertRaises(ValueError):
                    mod_menus.verify_inputs(root, wrong)
                wrong[rel] = (len(original), '0'*64)
                with self.assertRaisesRegex(ValueError, 'mismatch'):
                    mod_menus.verify_inputs(root, wrong)
            with self.assertRaises(ValueError):
                mod_menus.verify_inputs(root, {k:v for k,v in records.items() if 'xx-YY' not in k})


if __name__ == '__main__':
    unittest.main()
