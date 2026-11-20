"""Lists words in the DOL equal to Wii hardware base addresses (pointer variables to the GX FIFO etc.).
Usage: python3 hw-pointers.py main.dol"""
import struct, sys

d = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', d[0:72])
addrs = struct.unpack('>18I', d[72:144])
sizes = struct.unpack('>18I', d[144:216])
WANT = {0xCC008000: 'GX FIFO', 0xCC000000: 'CP', 0xCD000000: 'Hollywood', 0xCD800000: 'Hollywood mirror',
        0xCC002000: 'VI', 0xCC005000: 'DSP', 0xCC003000: 'PI', 0xCC001000: 'PE'}
for j in range(18):
    if not sizes[j]:
        continue
    seg = d[offs[j]:offs[j] + sizes[j]]
    for k in range(0, len(seg) - 3, 4):
        w = struct.unpack('>I', seg[k:k + 4])[0]
        if w in WANT:
            print(f"{'code' if j < 7 else 'data'} {addrs[j] + k:08X}: {w:08X} ({WANT[w]})")
