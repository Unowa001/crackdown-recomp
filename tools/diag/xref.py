# Find big-endian 32-bit occurrences of values in a --dump_image file: xref.py image.bin 822BF268 [...]
import sys
img=open(sys.argv[1],'rb').read()
for v in sys.argv[2:]:
    b=int(v,16).to_bytes(4,'big'); i=img.find(b); hits=[]
    while i>=0: hits.append(f'{0x82000000+i:08X}'); i=img.find(b,i+1)
    print(v, ' '.join(hits[:20]))
