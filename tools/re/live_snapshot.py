"""Read bounded guest-memory samples from one explicitly identified Windows process.

Uses PROCESS_VM_READ/QUERY_INFORMATION only. Never injects code or writes process
memory. Output is local evidence under ignored out/ap-discovery.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int,required=True)
    parser.add_argument('--base',type=lambda s:int(s,16),required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    kernel.OpenProcess.restype=wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes=[wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype=wintypes.BOOL
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    handle=kernel.OpenProcess(0x0410,False,args.pid)
    if not handle: raise ctypes.WinError(ctypes.get_last_error())
    args.output.mkdir(parents=True,exist_ok=True)
    sampled={}
    def read(address,size):
        if not 0<address<=0xffffffff or not 0<size<=65536 or address+size>0x100000000:
            raise ValueError('Invalid bounded guest range')
        buf=ctypes.create_string_buffer(size); received=ctypes.c_size_t()
        if not kernel.ReadProcessMemory(handle,args.base+address,buf,size,ctypes.byref(received)) or received.value!=size:
            raise ctypes.WinError(ctypes.get_last_error())
        data=buf.raw
        sampled[f'{address:08X}-{size:X}.bin']=data
        return data
    def word(address): return struct.unpack('>I',read(address,4))[0]
    result={'pid':args.pid,'host_virtual_base':f'{args.base:016X}','bosses':[],'errors':[]}
    try:
        globals=read(0x82DE0000,65536)
        try:
            manager=struct.unpack_from('>I',globals,0x2678)[0]
            data=read(manager,2816)
            count=struct.unpack_from('>I',data,8)[0]
            if not 0<count<=40: raise ValueError('Supply-point count outside native capacity')
            result['supply_manager']=f'{manager:08X}'
            result['supply_points']=[{
                'index_candidate':i,
                'position':list(struct.unpack_from('>3f',data,64+64*i)),
                'required_level_candidate':data[2576+i],
                'unlocked_per_player_candidate':[data[2616+i],data[2656+i]],
                'visited_per_player_candidate':list(data[2712+2*i:2714+2*i])
            } for i in range(count)]
        except (OSError,ValueError) as err: result['errors'].append('supply points: '+str(err))
        try:
            manager=word(0x835D3F84)
            if not manager: raise ValueError('Armory singleton not initialized')
            data=read(manager,40)
            begin,end=struct.unpack_from('>2I',data,12)
            count=(end-begin)//112
            if not 0<count<=64 or end-begin!=112*count: raise ValueError('Armory vector layout mismatch')
            records=read(begin,end-begin)
            entries=[]
            for i in range(count):
                obj=records[112*i:112*(i+1)]
                capacity=struct.unpack_from('>I',obj,80)[0]
                address=struct.unpack_from('>I',obj,60)[0] if capacity>=8 else begin+112*i+60
                raw=read(address,128)
                stop=next((j for j in range(0,len(raw)-1,2) if raw[j:j+2]==b'\0\0'),len(raw))
                entries.append({'index_candidate':i,'native_id_candidate':struct.unpack_from('>I',obj,12)[0],
                                'category':struct.unpack_from('>I',obj,16)[0],
                                'name':raw[:stop].decode('utf-16-be',errors='replace'),
                                'starting_available':obj[8],'available':obj[9]})
            result['armory_manager']=f'{manager:08X}'
            result['armory_entries']=entries
        except (OSError,ValueError) as err: result['errors'].append('armory: '+str(err))
        boss_manager=struct.unpack_from('>I',globals,0x27F4)[0]
        result['boss_manager']=f'{boss_manager:08X}'
        try:
            data=read(boss_manager,304)
            end=struct.unpack_from('>I',data,192)[0]
            count=(end-boss_manager-208)//4
            if not 0<=count<=24 or end-boss_manager-208!=count*4: raise ValueError('Boss vector layout mismatch')
            result['boss_count']=count
            for index in range(count):
                address=struct.unpack_from('>I',data,208+4*index)[0]
                obj=read(address,176)
                ident=struct.unpack_from('>I',obj,32)[0]
                capacity=struct.unpack_from('>I',obj,60)[0]
                name_address=struct.unpack_from('>I',obj,40)[0] if capacity>=8 else address+40
                raw=read(name_address,128)
                end=next((i for i in range(0,len(raw)-1,2) if raw[i:i+2]==b'\0\0'),len(raw))
                name=raw[:end].decode('utf-16-be',errors='replace')
                boss={'id':ident,'address':f'{address:08X}','name':name,'dead_byte_148':obj[148],'flag_145':obj[145],
                      'position':list(struct.unpack_from('>3f',obj,128)),
                      'spawn_ref_156':f'{struct.unpack_from(">I",obj,156)[0]:08X}',
                      'actor_160':f'{struct.unpack_from(">I",obj,160)[0]:08X}'}
                spawn_ref=struct.unpack_from('>I',obj,156)[0]
                if spawn_ref:
                    try:
                        spawn=read(spawn_ref,1024)
                        vtable=struct.unpack_from('>I',spawn)[0]
                        boss['spawn_ref_vtable']=f'{vtable:08X}'
                        boss['spawn_state_getter_candidate']=f'{word(vtable+44):08X}'
                    except (OSError,ValueError): pass
                result['bosses'].append(boss)
        except (OSError,ValueError) as err: result['errors'].append('boss table: '+str(err))
        player=struct.unpack_from('>I',globals,0x3F3C)[0]
        result['primary_player']=f'{player:08X}'
        try:
            data=read(player,4500)
            result['player_orb_counters_candidate']={'hidden':struct.unpack_from('>I',data,4392)[0],
                                                       'agility':struct.unpack_from('>I',data,4396)[0]}
            component=struct.unpack_from('>I',data,292)[0]
            character=component-32
            result['character_candidate']=f'{character:08X}'
            vtable=word(character)
            getter=word(vtable+1220)
            result['skill_getter_candidate']=f'{getter:08X}'
            # Record character fields for inspecting the getter statically; no function is called.
            read(character,8192)
            if getter==0x82299350:
                skill_address=character+9056
                skills=read(skill_address,344)
                result['skill_set']=f'{skill_address:08X}'
                result['skills']=[{'index':i,'base_byte':skills[12+i],'level_byte':skills[17+i],'bonus_byte':skills[22+i],
                                 'xp_float_28':struct.unpack_from('>f',skills,28+4*i)[0],
                                 'xp_float_48':struct.unpack_from('>f',skills,48+4*i)[0]} for i in range(5)]
                rpg=word(0x82D990AC)
                result['rpg_globals']=f'{rpg:08X}'
                read(rpg,832)
                thresholds=read(rpg+548,200)
                result['thresholds_136_plus_raw_level']=[list(struct.unpack_from('>5I',thresholds,20*i)) for i in range(5)]
                result['thresholds_161_plus_raw_level']=[list(struct.unpack_from('>5I',thresholds,100+20*i)) for i in range(5)]
        except (OSError,ValueError) as err: result['errors'].append('player chain: '+str(err))
        try:
            manager=word(0x82DE3F34)
            data=read(manager,14040)
            count,hidden,agility=struct.unpack_from('>3I',data,4012)
            if count>1000: raise ValueError('Pickup array exceeds its static capacity')
            result['pickup_manager']=f'{manager:08X}'
            result['pickup_counts']={'total':count,'hidden':hidden,'agility':agility}
            orbs=[]
            for index in range(count):
                address=struct.unpack_from('>I',data,12+4*index)[0]
                obj=read(address,304)
                kind,field268=struct.unpack_from('>2I',obj,264)
                position=list(struct.unpack_from('>3f',obj,272))
                orbs.append({'save_index_candidate':index,'address':f'{address:08X}','type':kind,'field_268':field268,
                             'position':position,'available_bytes_288':list(obj[288:290])})
            result['orbs']=orbs
            result['pickup_type_counts']={str(kind):sum(o['type']==kind for o in orbs) for kind in sorted({o['type'] for o in orbs})}
        except (OSError,ValueError) as err: result['errors'].append('pickups: '+str(err))
        try:
            begin,end=struct.unpack('>2I',read(0x82C631F8,8))
            count=(end-begin)//4
            if not 0<=count<=4096: raise ValueError('Resource vector range mismatch')
            pointers=read(begin,count*4) if count else b''
            result['resource_count']=count
            matches=[]
            for i in range(count):
                address=struct.unpack_from('>I',pointers,4*i)[0]
                if not address: continue
                try:
                    obj=read(address,0x120)
                    # Existing RE notes identify inline UTF-16 name and type-name spans.
                    name=obj[4:0x10C].decode('utf-16-be',errors='ignore').split('\0',1)[0]
                    if any(s in name.lower() for s in ('orb','agility','pickup','boss','gang','rpg')):
                        matches.append({'address':f'{address:08X}','name_candidate':name})
                except (OSError,ValueError): continue
            result['resource_name_candidates']=matches
        except (OSError,ValueError) as err: result['errors'].append('resources: '+str(err))
    finally:
        kernel.CloseHandle(handle)
    for name,data in sampled.items(): (args.output/name).write_bytes(data)
    (args.output/'snapshot.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    summary={k:result[k] for k in ('pid','errors','player_orb_counters_candidate','skills','pickup_counts') if k in result}
    summary['supply_point_count']=len(result.get('supply_points',[]))
    summary['armory_entry_count']=len(result.get('armory_entries',[]))
    summary['boss_count']=len(result['bosses'])
    print(json.dumps(summary,indent=2,ensure_ascii=True))

if __name__=='__main__': main()
