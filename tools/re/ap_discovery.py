"""Read-only string/xref/function discovery for the supported Crackdown guest image.

Writes nothing by default; redirect output into ignored out/ap-discovery when needed.
All reported candidates need runtime confirmation before use as hooks.
"""
import argparse
import bisect
import json
from pathlib import Path
import re
import struct

BASE = 0x82000000

def function_starts(generated):
    text = (generated / 'crackdown_init.cpp').read_text(encoding='utf-8')
    return sorted({int(s, 16) for s in re.findall(r'\{ 0x([0-9A-Fa-f]{8}), sub_', text)})

def owner(starts, address):
    i = bisect.bisect_right(starts, address) - 1
    return f'sub_{starts[i]:08X}' if i >= 0 else None

def strings(image, pattern):
    match = re.compile(pattern, re.I)
    for encoding, regex in [('ascii', rb'[\x20-\x7e]{5,}'), ('utf-16-be', rb'(?:\x00[\x20-\x7e]){5,}'), ('utf-16-le', rb'(?:[\x20-\x7e]\x00){5,}')]:
        for found in re.finditer(regex, image):
            if encoding != 'ascii' and found.start() % 2:
                continue
            text = found.group().decode(encoding)
            if match.search(text):
                yield dict(address=f'{BASE + found.start():08X}', encoding=encoding, text=text)

def xrefs(image, starts, targets):
    # Exact data pointers, then lis + signed displacement/ori pairs in generated code range.
    for target in targets:
        needle = struct.pack('>I', target)
        offset = image.find(needle)
        while offset >= 0:
            yield dict(kind='pointer', target=f'{target:08X}', address=f'{BASE+offset:08X}')
            offset = image.find(needle, offset + 1)
    low = starts[0] - BASE
    high = min(len(image), starts[-1] - BASE + 0x10000)
    words = struct.unpack(f'>{(high-low)//4}I', image[low:low+(high-low)//4*4])
    targets = set(targets)
    for i, word in enumerate(words):
        if word >> 26 != 15 or (word >> 16) & 31:
            continue
        reg = (word >> 21) & 31
        upper = (word & 0xffff) << 16
        for j in range(i+1, min(i+12, len(words))):
            following = words[j]
            op = following >> 26
            ra = (following >> 16) & 31
            if ra == reg and op in (14, 24, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54):
                displacement = following & 0xffff
                value = (upper + (displacement if op == 24 else displacement - 0x10000 if displacement & 0x8000 else displacement)) & 0xffffffff
                if value in targets:
                    address = BASE + low + 4*j
                    yield dict(kind='code-candidate', target=f'{value:08X}', address=f'{address:08X}', function=owner(starts,address))
            # Stop when an obvious assignment clobbers the register containing the upper bits.
            if op in (14, 15, 32, 34, 40) and ((following >> 21) & 31) == reg:
                break

def function_text(generated, name):
    name = name if name.startswith('sub_') else 'sub_' + name.upper()
    marker = f'DEFINE_REX_FUNC({name})'
    for path in generated.glob('crackdown_recomp.*.cpp'):
        text = path.read_text(encoding='utf-8')
        start = text.find(marker)
        if start < 0:
            continue
        end = text.find('DEFINE_REX_FUNC(', start + len(marker))
        return path, text[start:end if end >= 0 else None]
    raise SystemExit(f'Function not found: {name}')

def show_function(generated, name):
    path, text = function_text(generated,name)
    print(f'# {path.name}: {name}')
    for line in text.splitlines():
            if line.strip().startswith('// ') or line.startswith('loc_'):
                print(line.strip())

def bindings(image, generated, name):
    """Conservative constant tracking for common script-registration idioms.

    Not a PPC emulator: results are static candidates, printed with the registrar
    and instruction sequence so they can be checked against the disassembly.
    """
    _, text = function_text(generated,name)
    regs, stack, allocated_callback = {}, {}, None
    def string(address):
        offset = address - BASE
        if offset < 0 or offset >= len(image):
            return None
        if image[offset]==0:
            end=offset
            while end+1<len(image) and image[end:end+2]!=b'\0\0': end+=2
            try: return image[offset:end].decode('utf-16-be')
            except UnicodeError: return None
        raw=image[offset:image.find(b'\0',offset)]
        if not 3 <= len(raw) <= 100 or not all(32 <= c < 127 for c in raw):
            return None
        return raw.decode('ascii')
    for line in text.splitlines():
        if not line.strip().startswith('// '):
            continue
        instruction = line.strip()[3:]
        match = re.fullmatch(r'(lis|li) r(\d+),(-?\d+)',instruction)
        if match:
            op,r,n=match.groups(); regs[int(r)]=(int(n) << (16 if op=='lis' else 0)) & 0xffffffff
            continue
        match = re.fullmatch(r'(addi|ori) r(\d+),r(\d+),(-?\d+)',instruction)
        if match:
            op,d,s,n=match.groups(); d,s,n=int(d),int(s),int(n)
            if s in regs:
                regs[d]=((regs[s]+n) if op=='addi' else regs[s]|n)&0xffffffff
            else:
                regs.pop(d,None)
            continue
        match = re.fullmatch(r'mr r(\d+),r(\d+)',instruction)
        if match:
            d,s=map(int,match.groups())
            if s in regs: regs[d]=regs[s]
            else: regs.pop(d,None)
            continue
        match = re.fullmatch(r'stw r(\d+),(-?\d+)\(r(\d+)\)',instruction)
        if match:
            r,off,base=map(int,match.groups())
            if base==1:
                if r in regs: stack[off]=regs[r]
                else: stack.pop(off,None)
            if base==3 and off==4 and r in regs and allocated_callback:
                label=string(regs[r]); callback=allocated_callback
                if label and 0x82100000 <= callback < 0x82b00000:
                    yield dict(name=label,function=f'sub_{callback:08X}',registrar=name,evidence='callback stored at stack+80; name stored at binding+4')
            continue
        if instruction.startswith('bl '):
            if instruction == 'bl 0x82321728' and 4 in regs and 10 in regs:
                label=string(regs[4]); callback=regs[10]
                if label and 0x82100000<=callback<0x82b00000:
                    yield dict(name=label,function=f'sub_{callback:08X}',registrar=name,evidence='console name in r4; callback in r10')
            if instruction == 'bl 0x82381650':
                callback=stack.get(80)
                allocated_callback=callback if callback and 0x82100000<=callback<0x82b00000 else None
            if 3 in regs and 80 in stack:
                label=string(regs[3]); callback=stack[80]
                if label and 0x82100000 <= callback < 0x82b00000:
                    yield dict(name=label,function=f'sub_{callback:08X}',registrar=name,evidence='name passed in r3; callback stored at stack+80')
            for r in range(13): regs.pop(r,None)
            continue
        match = re.match(r'(?:lwz|lbz|lhz|ld|rlwinm|clrlwi|add|subf|mulli) r(\d+),',instruction)
        if match: regs.pop(int(match.group(1)),None)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('out/ap-discovery/guest-image.bin'))
    parser.add_argument('--generated', type=Path, default=Path('crackdown/generated/default'))
    sub = parser.add_subparsers(dest='command', required=True)
    find = sub.add_parser('strings')
    find.add_argument('pattern')
    refs = sub.add_parser('xrefs')
    refs.add_argument('addresses', nargs='+', type=lambda x: int(x,16))
    function = sub.add_parser('function')
    function.add_argument('name')
    bind = sub.add_parser('bindings')
    bind.add_argument('name')
    caller = sub.add_parser('callers')
    caller.add_argument('addresses',nargs='+',type=lambda x:int(x,16))
    args = parser.parse_args()
    if args.command == 'function':
        show_function(args.generated, args.name)
        return
    image = args.image.read_bytes()
    if args.command == 'callers':
        starts=function_starts(args.generated)
        low=starts[0]-BASE
        high=min(len(image),starts[-1]-BASE+0x10000)
        targets=set(args.addresses)
        rows=[]
        for offset in range(low,high-3,4):
            word=struct.unpack_from('>I',image,offset)[0]
            if word>>26!=18: continue
            delta=word&0x3fffffc
            if delta&0x2000000: delta-=0x4000000
            target=(delta if word&2 else BASE+offset+delta)&0xffffffff
            if target in targets:
                rows.append(dict(target=f'{target:08X}',address=f'{BASE+offset:08X}',function=owner(starts,BASE+offset),kind='call' if word&1 else 'branch'))
    elif args.command == 'bindings':
        rows=bindings(image,args.generated,args.name)
    else:
        rows = strings(image,args.pattern) if args.command == 'strings' else xrefs(image,function_starts(args.generated),args.addresses)
    for row in rows:
        print(json.dumps(row,ensure_ascii=False))

if __name__ == '__main__':
    main()
