# Print floats at guest addresses from a --dump_image file: fconst.py image.bin base_hex off [off ...]
import struct,sys
img=open(sys.argv[1],'rb').read(); base=int(sys.argv[2],16)
for o in sys.argv[3:]:
    a=base+int(o,0); b=img[a-0x82000000:a-0x82000000+4]
    print(f'{o:>7} {a:08X} {struct.unpack(">f",b)[0]!r} {b.hex()}')
