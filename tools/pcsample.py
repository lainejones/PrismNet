#!/usr/bin/env python3
"""pcsample.py SAMPLES SYMFILE CODEBASE CODESIZE [TOP] [NAME=BASE:SIZE ...] - flat profile from pcsample.

Finds which word of the saved context is the PC (the one inside the code nearly every time) and maps
PCs inside CODEBASE..+CODESIZE onto SYMFILE's functions (an unstripped build with the same code as the
running program; out/PrismNet.sym for the core). NAME=BASE:SIZE names other code ranges (another
program, a library), from NetHarness TASKS / LIBS. Both watched tasks are reported, task 2 too: socket
calls run core code in the caller's task. Run with MSYS_NO_PATHCONV=1 from Git Bash."""
import struct, subprocess, sys, collections, bisect

NL = 24
path, symf, base, size = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0)
top = int(sys.argv[5]) if len(sys.argv) > 5 else 25
ranges = []
for a in sys.argv[6:]:
    name, r = a.split('=')
    b, s = r.split(':')
    ranges.append((name, int(b, 0), int(s, 0)))
data = open(path, 'rb').read()
recs = {1: [], 2: []}
none = i = 0
while i + 4 <= len(data):
    h = struct.unpack('>I', data[i:i + 4])[0]
    i += 4
    if h == 0:
        none += 1
        continue
    recs[h >> 24].append(struct.unpack('>%dI' % NL, data[i:i + NL * 4]))
    i += NL * 4
inside = lambda v: base <= v < base + size
allr = recs[1] + recs[2]
score = [sum(inside(r[k]) for r in recs[1]) for k in range(NL)]
pck = max(range(NL), key=lambda k: score[k])
print('ticks with neither task ready: %d; task 1: %d, task 2: %d records; PC = word %d'
      % (none, len(recs[1]), len(recs[2]), pck))
nm = subprocess.run(['wsl', '-e', 'bash', '-lc', 'export PATH=/opt/amiga/bin:$PATH; m68k-amigaos-nm -n ' + symf],
                    capture_output=True, text=True).stdout
syms = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in nm.splitlines()) if len(p) == 3 and p[1] in 'tT')
addrs = [a for a, _ in syms]

def where(pc):
    if inside(pc):
        j = bisect.bisect_right(addrs, pc - base) - 1
        return syms[j][1] if j >= 0 else '?'
    for name, b, s in ranges:
        if b <= pc < b + s:
            return '[' + name + ']'
    return '[elsewhere: OS/ROM/libraries]'

for k in (1, 2):
    if not recs[k]:
        continue
    hits = collections.Counter(where(r[pck]) for r in recs[k])
    n = len(recs[k])
    print('\n== task %d: %d samples' % (k, n))
    for name, c in hits.most_common(top):
        print('%6.1f%%  %5d  %s' % (100.0 * c / n, c, name))
