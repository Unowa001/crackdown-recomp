# Per-function inventory of 30 fps assumptions and of the dev 60 Hz / clock paths.
# Usage: python -I tools/fpsaudit.py <guest image from --dump_image> crackdown/generated/default > out.txt
# Lists, per function, loads of 30 fps-looking float constants (1/30, 0.03, 33, 1/15, ...)
# and reads of the clock object [0x82DE25C0] (+13 locked, +14, +16 frame counter), the
# variable-ms flag 0x82DE3FB0, the step 0x820ED704 and the 1/60 constant 0x820ED634.
# Tracks lis/addi/ori only within straight-line code, so it misses some references.
import re,sys,glob,struct,collections
img=open(sys.argv[1],'rb').read(); BASE=0x82000000
def rd(a,n):
    o=a-BASE
    return img[o:o+n] if 0<=o<=len(img)-n else None
fn=re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
ins=re.compile(r'^// (\S+) ?(.*)$')
FCONST={'1/30':1/30,'0.03':0.03,'0.033':0.033,'33':33.0,'30':30.0,'1/60':1/60,'60':60.0,'0.0333':0.0333,'2/30':2/30,'33.33':100/3,'1000/30':1000/30}
NAMED={0x820ED704:'STEP',0x820ED634:'C1/60',0x82DE3FB0:'VARFLAG',0x82D99228:'VARMS',0x82BAA330:'FRAMECAP'}
CLOCK=0x82DE25C0
def match(v):
    for n,t in FCONST.items():
        if v is not None and v==v and abs(v-t)<=abs(t)*2e-6: return n
out=collections.defaultdict(collections.Counter)
def note(f,a,kind):
    if a in NAMED: out[f][NAMED[a]]+=1; return
    if kind in('lfs','lfsx','lfsu'):
        b=rd(a,4); v=struct.unpack('>f',b)[0] if b else None
    elif kind in ('lfd','lfdu','lfdx'):
        b=rd(a,8); v=struct.unpack('>d',b)[0] if b else None
    else: return
    n=match(v)
    if n: out[f][f'{n}@{a:08X}']+=1
for path in glob.glob(sys.argv[2]+'/*.cpp'):
    cur=None; R={}
    for line in open(path,errors='ignore'):
        m=fn.match(line)
        if m: cur=m.group(1); R={}; continue
        s=line.strip()
        if not cur or not s.startswith('//'): continue
        if s.startswith('// loc_') : continue
        m=ins.match(s)
        if not m: continue
        op,args=m.group(1),[x.strip() for x in m.group(2).split(',')]
        try:
            if op=='lis': R[args[0]]=('c',(int(args[1])<<16)&0xFFFFFFFF); continue
            if op in('addi','addic') and args[1] in R and R[args[1]][0]=='c':
                R[args[0]]=('c',(R[args[1]][1]+int(args[2]))&0xFFFFFFFF); continue
            if op=='ori' and args[1] in R and R[args[1]][0]=='c':
                R[args[0]]=('c',R[args[1]][1]|int(args[2])); continue
            mm=re.match(r'(-?\d+)\((r\d+)\)',args[1]) if len(args)>1 else None
            if mm:
                off=int(mm.group(1)); base=mm.group(2); v=R.get(base)
                if v and v[0]=='c':
                    a=(v[1]+off)&0xFFFFFFFF
                    if a==CLOCK and op=='lwz': R[args[0]]=('clk',0); continue
                    if a in NAMED: out[cur][NAMED[a]+('.w' if op.startswith('st') else '')]+=1
                    note(cur,a,op)
                elif v and v[0]=='clk' and op in('lbz','lwz','lhz','stb','stw'):
                    out[cur][f'CLOCK+{off}'+('.w' if op.startswith('st') else '')]+=1
                if args[0] in R and not op.startswith('st'): R.pop(args[0],None)
                continue
            if args and args[0] in R and op not in ('cmpwi','cmplwi','cmpw','cmplw','b','bl','beq','bne','blt','bgt','ble','bge') and not op.startswith('st'):
                R.pop(args[0],None)
        except Exception: pass
        if op in('bl','b','bctrl','blr'): pass
for f in sorted(out):
    print(f, ' '.join(f'{k}x{v}' if v>1 else k for k,v in sorted(out[f].items())))
