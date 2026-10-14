#!/usr/bin/env python3
"""Build original-verified PC menu packages and audited patches under out/xui.

Run as: python3 -m tools.xui.mod_menus ~/DecompProject/game
No input is changed. New labels are English in every supplied locale.
"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import subprocess
import sys

if __package__:
    from . import edit, xpatch, xur, xus, xzp
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.xui import edit, xpatch, xur, xus, xzp

REPO = Path(__file__).resolve().parents[2]
MANIFEST_SOURCE = 'wip/launcher:tools/game-manifest.txt'


def manifest_records(text):
    records = {}
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        parts = line.split('\t')
        if len(parts) != 3 or parts[0] in records:
            raise ValueError('invalid or duplicate manifest row')
        path, size, sha = parts
        if path.startswith('/') or '\\' in path or any(p in ('', '.', '..') for p in path.split('/')):
            raise ValueError('unsafe manifest path')
        if sha != '-' and (len(sha) != 64 or any(c not in '0123456789abcdef' for c in sha)):
            raise ValueError('invalid manifest SHA256')
        records[path] = (int(size), sha)
    return records


def verify_inputs(game, records):
    """Verify all recipe inputs before parsing, modifying or writing any output."""
    game = game.resolve()
    paths = [game/'XUIFrontEnd/menuHUD_3.xzp', game/'XUIFrontEnd/Scene.xzp',
             game/'Local/en-US/XUIFrontEnd/Scene.xzp']
    paths += sorted((game/'Local').glob('*/XUIFrontEnd/Scene.xzp'))
    inputs = {}
    for path in dict.fromkeys(paths):
        if not path.resolve().is_relative_to(game):
            raise ValueError('game input symlink escapes game root')
        rel = path.relative_to(game).as_posix()
        expected = records.get(rel)
        if expected is None or expected[1] == '-':
            raise ValueError(f'no trusted original SHA256 for {rel} in {MANIFEST_SOURCE}')
        original = path.read_bytes()
        if len(original) != expected[0] or hashlib.sha256(original).hexdigest() != expected[1]:
            raise ValueError(f'original size/SHA256 mismatch: {rel}')
        inputs[rel] = original
    return inputs


def nav_cycle(editor, controls):
    for i, obj in enumerate(controls):
        editor.set_property(obj, 'NavUp', controls[i-1]['id'])
        editor.set_property(obj, 'NavDown', controls[(i+1) % len(controls)]['id'])


def modify_menu(data):
    editor = edit.Editor.parse(data)
    tab = editor.find('Menu/Menu/Tab1')
    source = editor.find('Menu/Menu/Tab1/displaySetup')
    clone = editor.clone(source, tab, 'pcSettings', tab['children'].index(source)+1)
    editor.set_property(clone, 'PressPath', '')
    editor.set_property(clone, 'Text', 'PC Settings')
    editor.clone_timelines(tab, source, clone)
    ids = ['controllerSettings', 'liveSetup', 'audioSetup', 'displaySetup', 'pcSettings', 'gameCredits']
    controls = [editor.find('Menu/Menu/Tab1/'+name) for name in ids]
    nav_cycle(editor, controls)
    # Five original 37px rows at 38px pitch occupy 189px. Six rows at 31.5px
    # height/pitch occupy the same 189px, leaving the header/footer untouched.
    for i, obj in enumerate(controls):
        editor.set_property(obj, 'Position', [259.0, 372.6 + 31.5*i, 0.0])
        editor.set_property(obj, 'Height', 31.5)
    tab2 = editor.find('Menu/Menu/Tab2')
    quit = editor.find('Menu/Menu/Tab2/quit')
    desktop = editor.clone(quit, tab2, 'quitGame', tab2['children'].index(quit)+1)
    for name, value in [('PressPath', ''), ('Text', 'Quit to Desktop')]:
        editor.set_property(desktop, name, value)
    editor.clone_timelines(tab2, quit, desktop)
    controls2 = [editor.find('Menu/Menu/Tab2/'+name)
                 for name in ['campaign', 'timeTrials', 'DLC', 'extract', 'quit', 'quitGame']]
    nav_cycle(editor, controls2)
    for i, obj in enumerate(controls2):
        editor.set_property(obj, 'Position', [515.0, 372.6 + 31.5*i, 0.0])
        editor.set_property(obj, 'Height', 31.5)
    editor.set_property(editor.find('Menu/Menu/Tab2/mainMenuButton'), 'NavUp', 'quitGame')
    return editor.finish()


def pc_scene(data):
    """Screen 26: native DisplayOptions chrome, three radio rows, safe ctor dummies.

    Static evidence: claude_screen.md (2026-10-12), ctor 0x8266B928.
    Native input/visual behavior is unverified at runtime until the PC receipt.
    """
    editor = edit.Editor.parse(data)
    page = editor.find('DisplayOptions')
    editor.set_property(page, 'Id', 'options')
    editor.set_property(page, 'ClassOverride', 'scene')
    editor.set_property(page, 'DefaultFocus', 'framerate')
    for name in ('NavLeft', 'NavRight', 'NavUp', 'NavDown'):
        editor.set_property(page, name, 'options')
    title = editor.find('options/pageTitle')
    for name, value in [('Text', 'PC Settings'), ('Width', 590.0),
                        ('Height', 36.0), ('Visual', 'XuiLabel'), ('PointSize', 24.0)]:
        editor.set_property(title, name, value)
    # Make room above the original controls; keep the native collapse animation.
    editor.set_property(title, 'Position', [346.0, 289.0, 0.0])
    background = editor.find('options/background2')
    editor.set_property(background, 'Height', 282.0)
    editor.set_property(background, 'Position', [336.0, 422.5, 0.0])
    for timeline in page['timelines']:
        if timeline['target'] in ('pageTitle', 'background2'):
            for key in timeline['keyframes']:
                for i, prop in enumerate(key['properties']):
                    if prop['name'] == 'Position':
                        position = list(prop['value'])
                        position[1] -= 44.0
                        key['properties'][i] = editor.make_property('Position', 'v', position)

    def caption(ident, text, y, size=20.0):
        label = editor.clone(title, page, ident)
        # This game's Element class predates Hittable/DisableFocusRecursion.
        # Use verified Control fields; unsupported bits desynchronize XUR load.
        for name, value in [('Text', text), ('Width', 590.0), ('Visual', 'XuiLabel'),
                            ('Height', 26.0), ('PointSize', size), ('Position', [354.0, y, 0.0]),
                            ('Enabled', False), ('UnfocusedInput', False)]:
            editor.set_property(label, name, value)
        # Position and any native focus/nav keys must not restore old rows.
        for anim in editor.clone_timelines(page, title, label):
            keep = [i for i, d in enumerate(anim['definitions']) if d['name'] == 'Opacity']
            anim['definitions'] = [anim['definitions'][i] for i in keep]
            for key in anim['keyframes']:
                key['properties'] = [key['properties'][i] for i in keep]
        return label

    preset = editor.find('options/preset')
    controls = []
    for ident, label, texts, y in [
        ('framerate', 'Frame rate', ['60 fps', '30 fps (original)'], 351.0),
        ('resolution', 'Internal resolution', ['720p', '1440p'], 411.0),
        ('display', 'Display', ['Fullscreen', 'Windowed'], 471.0),
    ]:
        group = editor.clone(preset, page, ident)
        editor.set_property(group, 'Position', [345.0, y, 0.0])
        editor.set_property(group, 'Height', 36.0)
        for anim in editor.clone_timelines(page, preset, group):
            keep = [i for i, d in enumerate(anim['definitions']) if d['name'] == 'Opacity']
            anim['definitions'] = [anim['definitions'][i] for i in keep]
            for key in anim['keyframes']:
                key['properties'] = [key['properties'][i] for i in keep]
        for button, text in zip(group['children'], texts):
            editor.set_property(button, 'Text', text)
        controls.append(group)
        caption(ident+'Label', label, y-25.0)
    nav_cycle(editor, controls)
    for group in controls:
        for button in group['children']:
            editor.set_property(button, 'NavUp', edit.property_value(group, 'NavUp'))
            editor.set_property(button, 'NavDown', edit.property_value(group, 'NavDown'))
    caption('restartNotice', 'Changes apply the next time the game starts.', 525.0, 18.0)

    # The options ctor ALWAYS subscribes AudioSetup+52 and conditionally hides
    # AgentProfiles. Missing wrappers are dereferenced; disabled real wrappers
    # are required even though neither dummy is ever navigated to.
    agent = editor.clone(title, page, 'AgentProfiles')
    audio = editor.clone(editor.find('options/reset_button'), page, 'AudioSetup')
    for dummy in (agent, audio):
        for name, value in [('Text', ''), ('Show', False),
                            ('Opacity', 0.0), ('Enabled', False),
                            ('UnfocusedInput', False)]:
            editor.set_property(dummy, name, value)
        for name in ('NavLeft', 'NavRight', 'NavUp', 'NavDown'):
            editor.set_property(dummy, name, '')
    editor.set_property(audio, 'PressPath', '')
    # No footer helper exists on screen 26: hide unset DisplayOptions hints.
    editor.set_property(editor.find('options/footer'), 'Show', False)
    # Retain the native XuiBackButton/B key and give mouse users a visible Back.
    back = editor.find('options/XuiBackButton1')
    for name, value in [('Text', 'Back'), ('Opacity', 1.0),
                        ('Visual', 'new-subMenuButton2'), ('Width', 160.0), ('Height', 36.0),
                        ('Position', [354.0, 575.0, 0.0]), ('NavUp', 'display'),
                        ('NavDown', 'framerate')]:
        editor.set_property(back, name, value)
    retained = {'XuiBackButton1', 'footer', 'background2', 'pageTitle', 'AgentProfiles',
                'AudioSetup', 'restartNotice', 'framerate', 'resolution', 'display',
                'framerateLabel', 'resolutionLabel', 'displayLabel'}
    page['children'] = [c for c in page['children'] if c['id'] in retained]
    page['timelines'] = [t for t in page['timelines'] if t['target'] in retained]
    return editor.finish()


def pc_localization(data):
    """Future options.xus: preserve chrome metrics, supply English new paths.

    These localization patches are audited recipe outputs, not shipped yet.
    """
    records = []
    chrome = {'pageTitle', 'background2', 'XuiBackButton1'}
    for record in xus.parse(data):
        parts = record['path'].split('.')
        if parts[0] != 'DisplayOptions':
            raise ValueError('unexpected DisplayOptions localization root')
        if len(parts) > 2 and parts[1] in chrome and parts[-1] not in ('Text', 'Position', 'Width'):
            parts[0] = 'options'
            records.append(dict(path='.'.join(parts), value=record['value']))
    labels = {'pageTitle': 'PC Settings', 'framerateLabel': 'Frame rate',
              'resolutionLabel': 'Internal resolution', 'displayLabel': 'Display',
              'restartNotice': 'Changes apply the next time the game starts.',
              'XuiBackButton1': 'Back',
              'framerate.XuiRadioButton1': '60 fps', 'framerate.XuiRadioButton2': '30 fps (original)',
              'resolution.XuiRadioButton1': '720p', 'resolution.XuiRadioButton2': '1440p',
              'display.XuiRadioButton1': 'Fullscreen', 'display.XuiRadioButton2': 'Windowed'}
    for path, text in labels.items():
        edit.set_localized(records, 'options.'+path+'.Text', text)
    return xus.build(records)


def validate_package(data):
    entries = xzp.parse(data)
    if xzp.build(entries) != data:
        raise ValueError('package roundtrip mismatch')
    scenes = localizations = 0
    for entry in entries:
        if entry.name.lower().endswith('.xur'):
            scene = xur.parse(entry.data)
            if not scene.fully_parsed or scene.serialize() != entry.data:
                raise ValueError(f'incomplete or non-lossless {entry.name}: {scene.errors}')
            scenes += 1
        elif entry.name.lower().endswith('.xus'):
            if xus.build(xus.parse(entry.data)) != entry.data:
                raise ValueError(f'non-lossless {entry.name}')
            localizations += 1
    return dict(entries=len(entries), scenes=scenes, localizations=localizations)


def modify_package(original, standalone=False):
    entries = xzp.parse(original)
    modified, added = [], []
    found_menu = False
    for entry in entries:
        data = entry.data
        if entry.name == 'menuHud_3.xur' and standalone:
            data = modify_menu(data)
            found_menu = True
        elif entry.name == 'DisplayOptions.xur':
            added.append(xzp.Entry('options.xur', pc_scene(data)))
        elif entry.name == 'menuHud_3.xus':
            records = xus.parse(data)
            data = edit.set_localized(records, 'Menu.Menu.Tab1.pcSettings.Text', 'PC Settings')
            data = edit.set_localized(records, 'Menu.Menu.Tab1.pcSettings.PointSize', '-1.000000')
            data = edit.set_localized(records, 'Menu.Menu.Tab2.quitGame.Text', 'Quit to Desktop')
            data = edit.set_localized(records, 'Menu.Menu.Tab2.quitGame.PointSize', '-1.000000')
        elif entry.name == 'DisplayOptions.xus':
            added.append(xzp.Entry('options.xus', pc_localization(data)))
        modified.append(xzp.Entry(entry.name, data))
    existing_names = {e.name for e in entries}
    if any(e.name in existing_names for e in added) or len({e.name for e in added}) != len(added):
        raise ValueError('new scene/localization already exists or is ambiguous')
    if standalone and (not found_menu or added):
        raise ValueError('unexpected standalone menu package')
    if not standalone and not added:
        raise ValueError('package has no DisplayOptions scene/localization')
    return xzp.build(modified+added)


def run(game, manifest_text, output=REPO/'out/xui', manifest_source=MANIFEST_SOURCE):
    inputs = verify_inputs(game, manifest_records(manifest_text))
    output = xzp.output_root(output)
    protected = {(game.resolve()/rel).resolve() for rel in inputs}
    report = []
    for rel, original in inputs.items():
        print(f'Processing {rel}', flush=True)
        validate_package(original)
        result = modify_package(original, standalone=(rel == 'XUIFrontEnd/menuHUD_3.xzp'))
        parsed = validate_package(result)
        patch = xpatch.generate(original, result, {'options.xur': 'DisplayOptions.xur', 'options.xus': 'DisplayOptions.xus'})
        stats = xpatch.audit(original, patch)
        if stats['original_matching_literal_windows']:
            raise ValueError('patch literals contain original game spans')
        if xpatch.apply(original, patch) != result:
            raise ValueError('patch result mismatch')
        package_path = xzp.output_root(output/'modified/game'/rel)
        patch_rel = rel + '.xpatch'
        patch_path = xzp.output_root(output/'patches'/patch_rel)
        for target, data in ((package_path, result), (patch_path, patch)):
            if target in protected:
                raise ValueError('output would replace an input package')
            xzp.write_output(target, data)
        row = dict(path=rel, patch=patch_rel, original_sha256=hashlib.sha256(original).hexdigest(),
                   result_sha256=hashlib.sha256(result).hexdigest(), result_bytes=len(result), **parsed, **stats)
        report.append(row)
        print(f'  {stats["patch_bytes"]} patch bytes; {stats["literal_bytes"]} literal bytes; '
              f'{stats["original_matching_literal_windows"]} original literal windows >=16 bytes', flush=True)
    report_path = xzp.output_root(output/'patches/report.json')
    if report_path in protected:
        raise ValueError('report would replace an input package')
    xzp.write_output(report_path, (json.dumps(dict(manifest_source=manifest_source, english_labels_all_locales=True,
                                                packages=report), indent=2)+'\n').encode('utf-8'))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--manifest', type=Path, help='explicit trusted manifest override (same tab-separated format)')
    args = parser.parse_args()
    text = args.manifest.read_text() if args.manifest else subprocess.run(
        ['git', 'show', MANIFEST_SOURCE], cwd=REPO, check=True, capture_output=True, text=True).stdout
    run(args.game, text, manifest_source=str(args.manifest) if args.manifest else MANIFEST_SOURCE)


if __name__ == '__main__':
    main()
