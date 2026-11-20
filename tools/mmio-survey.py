"""Finds every load/store to Wii hardware registers (lis rX,0xCC00/0xCC01/0xCD00/0xCD80 + d(rX))
and groups them by hardware block, with the functions (nearest entry at or before) that do it.
Usage: python3 mmio-survey.py main.dol entries.txt"""
import bisect, collections, struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
entries = sorted(int(l, 16) for l in open(sys.argv[2]) if l.strip())

BLOCKS = [  # (start, end, name)
    (0xCC000000, 0xCC001000, 'CP (command processor)'),
    (0xCC001000, 0xCC002000, 'PE (pixel engine)'),
    (0xCC002000, 0xCC003000, 'VI (video)'),
    (0xCC003000, 0xCC004000, 'PI (interrupts/FIFO)'),
    (0xCC004000, 0xCC005000, 'MI (memory)'),
    (0xCC005000, 0xCC006000, 'DSP/ARAM'),
    (0xCC006000, 0xCC008000, 'legacy DI/SI/EXI/AI (CC mirror)'),
    (0xCC008000, 0xCC009000, 'GX write-gather FIFO'),
    (0xCD000000, 0xCD006000, 'Hollywood (IPC, GPIO, timers)'),
    (0xCD006000, 0xCD006400, 'DI (disc)'),
    (0xCD006400, 0xCD006800, 'SI (serial)'),
    (0xCD006800, 0xCD006C00, 'EXI'),
    (0xCD006C00, 0xCD007000, 'AI (audio interface)'),
    (0xCD800000, 0xCD806000, 'Hollywood (IPC, GPIO, timers) mirror'),
    (0xCD806000, 0xCD807000, 'DI/SI/EXI/AI mirror'),
]

def block(a):
    for s, e, n in BLOCKS:
        if s <= a < e:
            return n
    return f'other {a & 0xFFFFF000:08X}'

LOADSTORE = set(range(32, 56))          # lwz..stfdu (integer and float loads/stores)
hits = collections.defaultdict(lambda: collections.defaultdict(set))
for j in range(7):
    if not sizes[j]:
        continue
    s, n, o = addrs[j], sizes[j], offs[j]
    hi = {}
    for k, (w,) in enumerate(struct.iter_unpack('>I', data[o:o + n])):
        a = s + 4 * k
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:
            if imm in (0xCC00, 0xCC01, 0xCD00, 0xCD80, 0xCD81, 0xCD01):
                hi[rd] = imm << 16
            else:
                hi.pop(rd, None)
            continue
        if op == 14 and ra in hi:                    # addi: register now holds the full address
            t = (hi[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            hi[rd] = t
            continue
        if op in LOADSTORE and ra in hi:
            t = (hi[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            i = bisect.bisect_right(entries, a) - 1
            fn = entries[i] if i >= 0 else 0
            hits[block(t)][t].add(fn)
        if op in (18, 19):
            hi.clear()

for b in sorted(hits, key=lambda x: -sum(len(v) for v in hits[x].values())):
    regs = hits[b]
    fns = set().union(*regs.values())
    print(f'{b}: {len(regs)} registers, {len(fns)} functions')
    for r in sorted(regs)[:6]:
        print(f'    {r:08X}: {len(regs[r])} functions')
