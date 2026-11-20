"""Finds stretches of text (printable ASCII, tabs, newlines, NULs) inside the DOL's code sections,
at least 32 bytes long, and removes entry points that fall inside them from an entries file.
Usage: python3 text-in-code.py main.dol entries.txt out_entries.txt"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])

def texty(b):
    return b in (9, 10, 13, 0) or 0x20 <= b < 0x7F

stretches = []
for j in range(7):
    if not sizes[j]:
        continue
    seg = data[offs[j]:offs[j] + sizes[j]]
    start = None
    printable = 0
    for i, b in enumerate(seg + b'\xff'):
        if i < len(seg) and texty(b):
            if start is None:
                start, printable = i, 0
            if b >= 0x20:
                printable += 1
        else:
            # long enough and mostly real characters (not runs of zero padding)
            if start is not None and i - start >= 32 and printable >= (i - start) * 3 // 4:
                stretches.append((addrs[j] + start, addrs[j] + i))
            start = None
total = sum(e - s for s, e in stretches)
print(f'text stretches in code: {len(stretches)}, {total} bytes')
for s, e in sorted(stretches, key=lambda x: x[0] - x[1])[:8]:
    print(f'  {s:08X}-{e:08X} ({e - s} bytes)')

entries = [int(l, 16) for l in open(sys.argv[2]) if l.strip()]
inside = [a for a in entries if any(s <= a < e for s, e in stretches)]
kept = [a for a in entries if a not in set(inside)]
print(f'entries {len(entries)}: {len(inside)} inside text, {len(kept)} kept')
print('  dropped:', ' '.join(f'{a:08X}' for a in inside[:30]))
with open(sys.argv[3], 'w') as f:
    for a in kept:
        f.write(f'{a:08X}\n')
