# Summarize --diag_vehicle "player" lines: speed over time (s since the first line, summing dt), peak, impacts.
# car_track.py <log> [<log> ...]
import re,sys
pat=re.compile(r'diag_vehicle: player (\w+) dt=([\d.]+) pos=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) v=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) \|vh\|=([\d.]+) dv=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) ([\d.]+).*rpm=(\d+) torque=(-?\d+) gear=(-?\d+)')
for path in sys.argv[1:]:
    t=0; nxt=0; peak=(0,0); big=[]
    print('==',path)
    for line in open(path,encoding='utf-8',errors='replace'):
        m=pat.search(line)
        if not m: 
            if 'diag_vehicle: impact' in line: big.append(line[line.index('impact'):].strip()[:160])
            continue
        g=m.groups(); dt=float(g[1]); t+=dt; vh=float(g[8]); dv=float(g[12])
        if vh>peak[0]: peak=(vh,t)
        if dv>2: big.append(f't={t:.2f} player dv={dv:.2f} v=({g[5]} {g[6]} {g[7]}) pos=({g[2]} {g[3]} {g[4]})')
        if t>=nxt:
            print(f'  t={t:5.2f} |vh|={vh:6.2f} y={float(g[3]):7.2f} vy={float(g[6]):6.2f} rpm={g[13]} gear={g[15]}'); nxt+=0.5
    print(f'  peak |vh|={peak[0]:.2f} at t={peak[1]:.2f}, {t:.2f} s logged')
    for b in big[:15]: print('  ',b)
