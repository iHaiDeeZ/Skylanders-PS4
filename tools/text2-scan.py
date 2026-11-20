"""Scans the DOL's .text2 section for function prologues (stwu r1,-N(r1)) and blr,
and checks whether anything in the DOL points at .text2 addresses.
Usage: python3 text2-scan.py main.dol symbols.txt"""
import re, struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
T2_ADDR, T2_SIZE = 0x804CF8E0, 0xB87E0
i = addrs.index(T2_ADDR)
seg = data[offs[i]:offs[i] + T2_SIZE]
words = [w for (w,) in struct.iter_unpack('>I', seg)]
prologues = [T2_ADDR + 4*k for k, w in enumerate(words) if (w & 0xFFFF8000) == 0x94218000]
blrs = sum(1 for w in words if w == 0x4E800020)
zeros = sum(1 for w in words if w == 0)
print(f'.text2 words {len(words)}: stwu-r1 prologues {len(prologues)}, blr {blrs}, zero words {zeros}')

known = set()
rx = re.compile(r'^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function')
names = {}
for line in open(sys.argv[2]):
    m = rx.match(line)
    if m:
        a = int(m.group(2), 16)
        known.add(a)
        names[a] = m.group(1)
missing = [a for a in prologues if a not in known]
print(f'prologues not yet known as functions: {len(missing)}')

# Pointers to .text2 stored anywhere in data sections (vtables, function tables).
ptrs = 0
for j in range(7, 18):
    if not sizes[j]:
        continue
    d = data[offs[j]:offs[j] + sizes[j]]
    for (w,) in struct.iter_unpack('>I', d[:len(d) // 4 * 4]):
        if T2_ADDR <= w < T2_ADDR + T2_SIZE:
            ptrs += 1
print(f'pointers into .text2 from data sections: {ptrs}')

# Direct branches (bl) from .text into .text2.
calls = 0
for j in range(7):
    if not sizes[j] or addrs[j] == T2_ADDR:
        continue
    d = data[offs[j]:offs[j] + sizes[j]]
    for k, (w,) in enumerate(struct.iter_unpack('>I', d)):
        if (w >> 26) == 18 and (w & 3) == 1:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = addrs[j] + 4*k + li
            if T2_ADDR <= tgt < T2_ADDR + T2_SIZE:
                calls += 1
print(f'bl calls from other text into .text2: {calls}')
print('sample known .text2 names:', [names[a] for a in sorted(known) if T2_ADDR <= a < T2_ADDR + T2_SIZE][:10])
