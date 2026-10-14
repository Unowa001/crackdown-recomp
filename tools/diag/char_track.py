# Player track from a --diag_char_proxy log: char_track.py <log> [step_s]
# Prints time since the gameplay segment started, position, output velocity, and snap times.
import re,sys
from datetime import datetime
step=float(sys.argv[2]) if len(sys.argv)>2 else 0.5
t0=None; snaps=[]; rows=[]; tele=None
for line in open(sys.argv[1],errors='ignore'):
    try: ts=datetime.strptime(line[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()
    except ValueError: continue
    if 'gameplay segment starts' in line: t0=ts
    if t0 is None: continue
    if 'teleport:' in line: tele=ts-t0
    if 'ground snap' in line: snaps.append(ts-t0)
    m=re.search(r'vout=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) [\d.]+ pos=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\)',line)
    if m: rows.append((ts-t0,)+tuple(map(float,m.groups())))
if tele is not None: print(f'teleport at {tele:.2f}')
print('snap times:', ' '.join(f'{s:.1f}' for s in snaps))
last=-9
for r in rows:
    if r[0]-last>=step:
        last=r[0]; h=(r[1]**2+r[3]**2)**.5
        print(f'{r[0]:5.2f} pos=({r[4]:8.2f} {r[5]:7.2f} {r[6]:8.2f}) v=({r[1]:6.2f} {r[2]:6.2f} {r[3]:6.2f}) h={h:5.2f}')
