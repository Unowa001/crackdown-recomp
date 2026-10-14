"""Verify the recorded integration runs for the first in-process AP contract probe.

Run after the commands in docs/re-ap-probe.md. Reads local evidence only; does not
boot the game or modify any saves. Failures are errors, including under python -O.
"""
import argparse
import json
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_run(root, name, orb, agility_level, no_xp, observed):
    before = json.loads((root / f'{name}-snapshot/snapshot.json').read_text())
    after = json.loads((root / f'{name}-after-snapshot/snapshot.json').read_text())
    log = (root / f'{name}.log').read_text()
    for sample in (before, after):
        require(not sample['errors'], f'{name}: sampling errors {sample["errors"]}')
        require(sample['pickup_counts'] == {'total': 800, 'hidden': 300, 'agility': 500},
                f'{name}: unexpected orb catalog')
    metadata = lambda s: [(o['save_index_candidate'], o['type'], o['position']) for o in s['orbs']]
    require(metadata(before) == metadata(after), f'{name}: orb metadata changed')
    changed = [i for i, (a, b) in enumerate(zip(before['orbs'], after['orbs']))
               if a['available_bytes_288'] != b['available_bytes_288']]
    require(changed == [orb], f'{name}: expected only orb {orb}, found {changed}')
    require(before['orbs'][orb]['available_bytes_288'] == [1, 1] and
            after['orbs'][orb]['available_bytes_288'] == [0, 1], f'{name}: collection flags')
    expected_counts = {'hidden': int(orb == 508), 'agility': int(orb == 178)}
    require(after['player_orb_counters_candidate'] == expected_counts, f'{name}: pickup counters')
    events = re.findall(r'ap_probe: orb collected ordinal=(\d+) type=(\d+) player=(\d+)', log)
    expected = [(str(orb), str(after['orbs'][orb]['type']), '0')] if observed else []
    require(events == expected, f'{name}: duplicate/missing collection observation {events}')
    require([s['level_byte'] for s in after['skills']] == [agility_level, 1, 1, 1, 1],
            f'{name}: unexpected skill levels')
    require([s['base_byte'] for s in after['skills']] == [agility_level, 1, 1, 1, 1],
            f'{name}: effective/base skill fields not synchronized')
    if no_xp:
        require(all(s['xp_float_28'] == 0 and s['xp_float_48'] == 0 for s in after['skills']),
                f'{name}: XP leaked through suppression')
        if orb == 508:
            skills = set(re.findall(r'ap_probe: suppressed bonus XP skill=(\d+)', log))
            require(skills == set('01234'), f'{name}: missing hidden reward bank evidence')
    else:
        require(after['skills'][0]['xp_float_28'] == 10 and after['skills'][0]['xp_float_48'] == 0,
                f'{name}: vanilla agility reward changed')
        require('suppressed' not in log, f'{name}: unexpected XP suppression')
    if not observed:
        require('ap_probe:' not in log, f'{name}: disabled probe produced an event')
    if agility_level == 2:
        require(log.count('ap_probe: native agility grant level=2') == 1,
                f'{name}: missing/duplicate explicit native grant')
    return metadata(after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('out/ap-discovery'))
    args = parser.parse_args()
    runs = [('probe-orb-vanilla', 178, 1, False, True),
            ('probe-orb-no-xp', 178, 2, True, True),
            ('probe-hidden-no-xp', 508, 1, True, True),
            ('probe-disabled60', 178, 1, False, False)]
    catalogs = [load_run(args.root, *run) for run in runs]
    require(all(c == catalogs[0] for c in catalogs), 'Fresh-run orb identity/order differed')
    name='probe-supply0-enter'
    before=json.loads((args.root/f'{name}-snapshot/snapshot.json').read_text())
    after=json.loads((args.root/f'{name}-after-snapshot/snapshot.json').read_text())
    log=(args.root/f'{name}.log').read_text()
    require(not before['errors'] and not after['errors'], 'Supply capture sampling errors')
    require(len(before['supply_points'])==len(after['supply_points'])==16, 'Supply catalog count')
    changes=[i for i,(a,b) in enumerate(zip(before['supply_points'],after['supply_points']))
             if a['unlocked_per_player_candidate']!=b['unlocked_per_player_candidate']]
    require(changes==[0] and before['supply_points'][0]['unlocked_per_player_candidate']==[0,0]
            and after['supply_points'][0]['unlocked_per_player_candidate']==[1,0], 'Supply capture flags')
    require(re.findall(r'ap_probe: supply unlocked index=(\d+) player=(\d+) caller=([0-9A-F]+)',log)
            == [('0','0','82533490')], 'Supply capture observation/caller')
    require(before['armory_entries']==after['armory_entries'], 'Starting armory changed on entry')
    require('ap_probe: weapon deposited' not in log, 'Starting equipment emitted a deposit transition')
    print('PASS: orb observations, five-skill XP suppression, native Agility grant, '
          '30/60 fps vanilla controls, safehouse capture and fresh-run catalog stability')


if __name__ == '__main__':
    main()
