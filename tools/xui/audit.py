#!/usr/bin/env python3
"""Audit all XUIZ packages and dump all embedded XURs under out/xui/."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import struct

try:
    from . import xzp, xur, xus
except ImportError:
    import xzp
    import xur
    import xus


def digest(data):
    return hashlib.sha256(data).hexdigest()


def audit(input_dir, outdir, locale_archive=None):
    root = xzp.output_root(outdir)
    root.mkdir(parents=True, exist_ok=True)
    report = dict(packages=[], localization=None)
    failures = 0
    for path in sorted(Path(input_dir).glob('*.xzp')):
        original = path.read_bytes()
        package = dict(file=str(path), sha256=digest(original), size=len(original), scenes=[])
        report['packages'].append(package)
        try:
            entries = xzp.parse(original)
            package['entries'] = len(entries)
            package['roundtrip'] = xzp.build(entries) == original
            if not package['roundtrip']:
                failures += 1
            xzp.extract(entries, root/path.stem)
            for entry in entries:
                if not entry.name.lower().endswith('.xur'):
                    continue
                result = dict(name=entry.name, size=entry.size, sha256=digest(entry.data))
                package['scenes'].append(result)
                target = xzp.output_root((root/path.stem/entry.name.replace('\\', '/')).with_suffix('.txt'))
                try:
                    scene = xur.parse(entry.data)
                    result.update(fully_parsed=scene.fully_parsed, errors=scene.errors,
                                  roundtrip=scene.serialize() == entry.data)
                    target.write_text(xur.dump(scene), encoding='utf-8')
                except (ValueError, IndexError, KeyError, RecursionError, struct.error) as exc:
                    result.update(fully_parsed=False, roundtrip=False, errors=[str(exc)])
                    target.write_text(f'ERROR: {exc}\nRaw XUR: {entry.data.hex()}\n', encoding='utf-8')
                if not result['fully_parsed'] or not result['roundtrip']:
                    failures += 1
            print(f'{path.name}: {len(entries)} entries; {sum(s["fully_parsed"] for s in package["scenes"])}/{len(package["scenes"])} XURs fully parsed')
        except (OSError, ValueError) as exc:
            package['error'] = str(exc)
            failures += 1
            print(f'{path.name}: ERROR {exc}')
        package['input_unchanged'] = digest(path.read_bytes()) == package['sha256']
        if not package['input_unchanged']:
            failures += 1
    if locale_archive:
        path = Path(locale_archive)
        original = path.read_bytes()
        entries = xzp.parse(original)
        locale = dict(file=str(path), sha256=digest(original), roundtrip=xzp.build(entries)==original, companions=[])
        report['localization'] = locale
        for entry in entries:
            if not entry.name.lower().endswith('.xus'):
                continue
            xzp.extract([entry], root/'localization'/path.parent.parent.name/path.stem)
            records = xus.parse(entry.data)
            identical = xus.build(records) == entry.data
            locale['companions'].append(dict(name=entry.name, overrides=len(records), roundtrip=identical))
            target = root/'localization'/path.parent.parent.name/path.stem/entry.name.replace('\\', '/')
            xzp.output_root(target.with_suffix('.xus.txt')).write_text('\n'.join(f'@0x{r["offset"]:x} {r["path"]} = {r["value"]!r}' for r in records)+'\n', encoding='utf-8')
            if not identical:
                failures += 1
        locale['input_unchanged'] = digest(path.read_bytes()) == locale['sha256']
        if not locale['roundtrip'] or not locale['input_unchanged']:
            failures += 1
    scenes = [s for p in report['packages'] for s in p['scenes']]
    report['totals'] = dict(packages=len(report['packages']), scenes=len(scenes),
                          unique_scenes=len({s['sha256'] for s in scenes}),
                          fully_parsed=sum(s['fully_parsed'] for s in scenes),
                          scene_roundtrips=sum(s['roundtrip'] for s in scenes), failures=failures)
    xzp.output_root(root/'report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report['totals']))
    if not report['packages']:
        raise ValueError('no .xzp inputs found')
    return 1 if failures else 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input_dir', type=Path)
    p.add_argument('--outdir', type=Path, default=Path(__file__).resolve().parents[2]/'out'/'xui')
    p.add_argument('--locale-archive', type=Path, help='optional locale Scene.xzp containing .xus overrides')
    args = p.parse_args()
    try:
        return audit(args.input_dir, args.outdir, args.locale_archive)
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
