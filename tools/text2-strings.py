"""Finds strings referenced by lis/addi (or lis/ori) pairs inside .text2 and prints the most common ones,
plus where the data pointers into .text2 live.
Usage: python3 text2-strings.py main.dol"""
import collections, re, struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
T2_ADDR, T2_SIZE = 0x804CF8E0, 0xB87E0

def read(addr, n):
    for j in range(18):
        if sizes[j] and addrs[j] <= addr < addrs[j] + sizes[j]:
            o = offs[j] + addr - addrs[j]
            return data[o:o + n]
    return None

def cstr(addr):
    b = read(addr, 120)
    if not b:
        return None
    b = b.split(b'\0')[0]
    if len(b) >= 5 and re.fullmatch(rb'[\x20-\x7e]+', b):
        return b.decode()
    return None

i = addrs.index(T2_ADDR)
words = [w for (w,) in struct.iter_unpack('>I', data[offs[i]:offs[i] + T2_SIZE])]
hi = {}
found = collections.Counter()
for w in words:
    op = w >> 26
    rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
    if op == 15 and ra == 0:          # lis rd, imm
        hi[rd] = imm << 16
    elif op == 14 and ra in hi:       # addi rd, ra, simm
        simm = imm - 0x10000 if imm & 0x8000 else imm
        s = cstr((hi[ra] + simm) & 0xFFFFFFFF)
        if s:
            found[s] += 1
for s, n in found.most_common(60):
    print(n, s)

print('--- data words pointing into .text2 (first 20):')
c = 0
for j in range(7, 18):
    if not sizes[j]:
        continue
    d = data[offs[j]:offs[j] + sizes[j] // 4 * 4]
    for k, (w,) in enumerate(struct.iter_unpack('>I', d)):
        if T2_ADDR <= w < T2_ADDR + T2_SIZE and c < 20:
            print(f'  at {addrs[j] + 4*k:08X} -> {w:08X}')
            c += 1
