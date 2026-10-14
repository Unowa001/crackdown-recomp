"""Export metadata only from matching live discovery snapshots, without assigning AP IDs."""
import argparse
import hashlib
import json
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('first',type=Path)
    parser.add_argument('second',type=Path)
    parser.add_argument('--output',type=Path,default=Path('out/ap-discovery/catalog.json'))
    args=parser.parse_args()
    first=json.loads(args.first.read_text(encoding='utf-8'))
    second=json.loads(args.second.read_text(encoding='utf-8'))
    if first.get('errors') or second.get('errors'): parser.error('Snapshot contains sampling errors')
    def orb_rows(s):
        return [{'save_index_candidate':o['save_index_candidate'],'type':o['type'],'position':o['position']} for o in s['orbs']]
    def boss_rows(s):
        return [{'id':b['id'],'name':b['name']} for b in s['bosses']]
    if orb_rows(first)!=orb_rows(second): parser.error('Orb order/type/position differs between runs')
    if boss_rows(first)!=boss_rows(second): parser.error('Boss IDs/names differ between runs')
    if len(first['orbs'])!=800 or first['pickup_counts']!={'total':800,'hidden':300,'agility':500}: parser.error('Unexpected pickup catalog counts')
    if len(first['bosses'])!=21 or {b['id'] for b in first['bosses']}!=set(range(21)): parser.error('Unexpected boss IDs')
    optional={}
    groups={
        'supply_points':('index_candidate','position','required_level_candidate'),
        'armory_entries':('index_candidate','native_id_candidate','category','name','starting_available')}
    for group,fields in groups.items():
        if group not in first and group not in second: continue
        if group not in first or group not in second: parser.error(f'{group} missing from one snapshot')
        rows=lambda s:[{k:row[k] for k in fields} for row in s[group]]
        if rows(first)!=rows(second): parser.error(f'{group} metadata differs between runs')
        if group=='supply_points' and len(first[group])!=16: parser.error('Unexpected supply-point count')
        if group=='armory_entries' and {r['native_id_candidate'] for r in first[group]}!=set(range(1,20)):
            parser.error('Unexpected armory IDs')
        optional[group]=rows(first)
    result={'schema':'crackdown-discovery-v0','status':'metadata; AP IDs unassigned; save-index stability requires reload validation',
            'supported_xex_sha256':'7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a',
            'evidence':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in [args.first,args.second]],
            'orbs':orb_rows(first),'bosses':boss_rows(first),
            'native_skill_indices':{'agility':0,'driving':1,'explosives':2,'strength':3,'firearms':4},
            'native_raw_levels':[1,2,3,4,5],
            'thresholds_136_plus_raw_level':first['thresholds_136_plus_raw_level'],
            'thresholds_161_plus_raw_level':first['thresholds_161_plus_raw_level']}
    result.update(optional)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(f'Wrote metadata for {len(result["orbs"])} orbs, {len(result["bosses"])} bosses, '
          f'{len(optional.get("supply_points",[]))} supply points and '
          f'{len(optional.get("armory_entries",[]))} armory entries to {args.output}')

if __name__=='__main__': main()
