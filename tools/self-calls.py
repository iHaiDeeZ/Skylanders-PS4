"""For each 'REAL DIFF function X' line in a jittest JT_AOT_REAL output, says whether X's code
(per the AOT manifest's ranges) contains a bl to X itself (self-recursion).
Usage: python3 self-calls.py main.dol manifest.txt real.out"""
import re, struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])

def word(a):
    for j in range(7):
        if sizes[j] and addrs[j] <= a < addrs[j] + sizes[j]:
            o = offs[j] + a - addrs[j]
            return struct.unpack('>I', data[o:o + 4])[0]
    return None

ranges = {}
for line in open(sys.argv[2]):
    parts = line.split('|')
    head = parts[0].split()
    fn = int(head[0], 16)
    rs = []
    for r in parts[1].split()[1:]:
        s, l = r.split(':')
        rs.append((int(s, 16), int(l, 16)))
    ranges[fn] = rs

seen = set()
for line in open(sys.argv[3]):
    m = re.match(r'REAL DIFF function ([0-9a-f]+) from [0-9a-f]+: (.*)', line)
    if not m:
        continue
    fn = int(m.group(1), 16)
    if fn in seen:
        continue
    seen.add(fn)
    selfcall = False
    for s, l in ranges.get(fn, []):
        for a in range(s, s + l, 4):
            w = word(a)
            if w is not None and (w >> 26) == 18 and (w & 3) == 1:
                li = w & 0x03FFFFFC
                li = li - 0x04000000 if li & 0x02000000 else li
                if a + li == fn:
                    selfcall = True
    print(f'{fn:08X} self-recursive={selfcall}  {m.group(2)[:70]}')
