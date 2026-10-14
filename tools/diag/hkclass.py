# Print Havok reflection member tables (hkClassMember arrays) containing a member name, from a --dump_image
# file: hkclass.py image.bin linearVelocity [...]. The dump is taken before static init, so the hkClass
# objects themselves (0x82D1xxxx) are still empty, but the member arrays are plain data.
# hkClassMember (20 bytes): +0 name, +4 class (bss), +8 enum, +12 type u8, +13 subtype u8, +14 cArraySize s16,
# +16 flags u16, +18 offset u16.
import struct,sys
img=open(sys.argv[1],'rb').read(); B=0x82000000
def w(a): return struct.unpack('>I',img[a-B:a-B+4])[0]
def h(a): return struct.unpack('>H',img[a-B:a-B+2])[0]
def s(a):
    if not B<=a<B+len(img): return None
    e=img.find(b'\0',a-B,a-B+80)
    t=img[a-B:e] if e>=0 else b''
    return t.decode('latin1') if t and all(32<=c<127 for c in t) else None
def ismember(a): return s(w(a)) is not None and img[a+12-B]<40 and w(a+8)==0 or (s(w(a)) and B<=w(a+8)<B+len(img))
for n in sys.argv[1+1:]:
    i=img.find(n.encode()+b'\0')
    while i>=0:
        if img[i-1]==0:
            p=(B+i).to_bytes(4,'big'); j=img.find(p)
            while j>=0:
                a=B+j
                if j%4==0 and img[a+12-B]<40 and h(a+18)<0x4000:
                    st=a
                    while ismember(st-20): st-=20
                    en=a
                    while ismember(en+20): en+=20
                    print(f'-- table {st:08X}..{en+20:08X}')
                    for m in range(st,en+20,20):
                        print(f'  +{h(m+18):<5} {s(w(m))}  type {img[m+12-B]} sub {img[m+13-B]} arr {h(m+14)}')
                j=img.find(p,j+1)
        i=img.find(n.encode()+b'\0',i+1)
