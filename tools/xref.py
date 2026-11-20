"""Finds code that builds a given address with lis + addi/ori (and data words equal to it), and names
the function each one is in (dtk symbols).
Usage: python3 xref.py main.dol symbols.txt address_hex"""
import bisect, re, struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
target = int(sys.argv[3], 16)
funcs = []
for line in open(sys.argv[2]):
    m = re.match(r'^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function', line)
    if m:
        funcs.append((int(m.group(2), 16), m.group(1)))
funcs.sort()
starts = [a for a, _ in funcs]

def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return f'{funcs[i][1]}+0x{a - funcs[i][0]:x}' if i >= 0 else '?'

for j in range(18):
    if not sizes[j]:
        continue
    seg = data[offs[j]:offs[j] + sizes[j]]
    hi = {}
    for k, (w,) in enumerate(struct.iter_unpack('>I', seg[:len(seg) // 4 * 4])):
        a = addrs[j] + 4 * k
        if j >= 7:
            if w == target:
                print(f'data {a:08X}')
            continue
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:
            hi[rd] = imm << 16
        elif op == 14 and ra in hi and (hi[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF == target:
            print(f'code {a:08X} in {owner(a)}')
        elif op == 24 and rd in hi and (hi[rd] | imm) == target:
            print(f'code {a:08X} in {owner(a)}')
