# Map guest addresses to the recompiled function containing them: addr2func.py <generated dir> addr [...]
import re,sys,bisect
starts=sorted(int(m,16) for m in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(sys.argv[1]+'/crackdown_init.cpp').read()))
for a in sys.argv[2:]:
    v=int(a,16); i=bisect.bisect_right(starts,v)-1
    print(f'{a} -> sub_{starts[i]:08X} +0x{v-starts[i]:X}')
