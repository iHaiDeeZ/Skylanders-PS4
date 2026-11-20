"""For each 16 KiB block of the code sections, prints how code-like it is: share of words that are
valid common PowerPC instructions, blr count, zero words, and words that look like pointers/floats.
Usage: python3 region-check.py main.dol [start_hex end_hex]"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xFFFFFFFF
COMMON = {14, 15, 32, 36, 18, 16, 19, 31, 24, 21, 11, 10, 34, 38, 40, 44, 48, 52, 50, 54, 63, 59, 4, 56, 60, 28, 25, 29, 7, 20, 46, 47, 37, 33, 12, 13, 8, 23, 26, 27, 35, 39, 41, 42, 45, 49, 51, 53, 55, 57, 61}
for j in range(7):
    if not sizes[j]:
        continue
    s, n, o = addrs[j], sizes[j], offs[j]
    for b in range(s, s + n, 0x4000):
        if b + 0x4000 <= lo or b >= hi:
            continue
        ws = [w for (w,) in struct.iter_unpack('>I', data[o + b - s:o + min(b + 0x4000, s + n) - s])]
        zero = sum(1 for w in ws if w == 0)
        blr = sum(1 for w in ws if w == 0x4E800020)
        ptr = sum(1 for w in ws if 0x80000000 <= w < 0x81800000)
        flt = sum(1 for w in ws if 0x3C000000 <= w < 0x48000000 and (w & 0xFFFF) == 0)
        stwu = sum(1 for w in ws if (w & 0xFFFF8000) == 0x94218000)
        print(f'{b:08X}: words {len(ws):5} zero {zero:5} blr {blr:4} stwu {stwu:4} ptr-like {ptr:5} float-like {flt:5}')
