# Find code that builds a guest address with lis+addi/ori (or lis + load/store displacement):
# strref.py image.bin <generated dir> addr [...]
import re,sys,bisect,struct
img=open(sys.argv[1],'rb').read()
starts=sorted(int(m,16) for m in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(sys.argv[2]+'/crackdown_init.cpp').read()))
lo,hi=starts[0]-0x82000000,starts[-1]-0x82000000+0x10000
words=struct.unpack(f'>{(hi-lo)//4}I',img[lo:hi])
targets={int(a,16) for a in sys.argv[3:]}
for i,w in enumerate(words):
    if w>>26!=15 or (w>>16)&31: continue          # lis rD,imm (addis rD,0,imm)
    rd=(w>>21)&31; hi16=(w&0xFFFF)<<16
    for j in range(i+1,min(i+12,len(words))):
        x=words[j]; op=x>>26; ra=(x>>16)&31
        if ra!=rd: continue
        if op in (14,24,32,34,36,38,40,44,48,50,52,54): # addi ori lwz lbz stw stb lhz sth lfs lfd stfs stfd
            d=x&0xFFFF
            v=(hi16+(d if op==24 else (d-0x10000 if d&0x8000 else d)))&0xFFFFFFFF
            if v in targets:
                a=0x82000000+lo+4*j; k=bisect.bisect_right(starts,a)-1
                print(f'{v:08X} at {a:08X} sub_{starts[k]:08X}')
