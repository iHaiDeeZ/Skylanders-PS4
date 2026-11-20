"""Counts the PowerPC/Gekko instruction forms in the reached code that matter for the recompiler:
SPR accesses (by SPR number), paired-single forms, locked-cache and cache ops, string/multiple
loads/stores, traps, syscalls, time base, and the 0xE0000000 locked-cache address range.
Only words inside functions reached from entries.txt are counted (data inside code is skipped).

Usage: python3 insn-survey.py main.dol entries.txt"""
import collections, struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
CODE = [(addrs[j], sizes[j], offs[j]) for j in range(7) if sizes[j]]
entries = sorted(int(l, 16) for l in open(sys.argv[2]) if l.strip())

def word(a):
    for s, n, o in CODE:
        if s <= a < s + n:
            return struct.unpack('>I', data[o + a - s:o + a - s + 4])[0]
    return None

# Reached instruction set: walk like entries.py (simplified: follow flow until blr/b-away/bctr/rfi).
entry_set = set(entries)
reached = set()
for e in entries:
    todo = [e]
    while todo:
        a = todo.pop()
        while a not in reached:
            w = word(a)
            if w is None or w == 0:
                break
            if a != e and a in entry_set:
                break
            reached.add(a)
            op = w >> 26
            if op == 18 and not (w & 1):
                li = w & 0x03FFFFFC
                li = li - 0x04000000 if li & 0x02000000 else li
                t = li if w & 2 else a + li
                if t in entry_set:
                    break
                a = t
                continue
            if op == 16 and not (w & 1):
                bd = w & 0xFFFC
                bd = bd - 0x10000 if bd & 0x8000 else bd
                todo.append(bd if w & 2 else a + bd)
                if ((w >> 21) & 0x14) == 0x14:
                    break
            if op == 19 and ((w >> 1) & 0x3FF) in (16, 528) and not (w & 1) and ((w >> 21) & 0x14) == 0x14:
                break
            if op == 19 and ((w >> 1) & 0x3FF) == 50:
                break
            a += 4

c = collections.Counter()
spr_read, spr_write = collections.Counter(), collections.Counter()
ps = collections.Counter()
lc_addr = 0
for a in reached:
    w = word(a)
    op = w >> 26
    xo10 = (w >> 1) & 0x3FF
    c['total'] += 1
    if op == 31:
        spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        if xo10 == 339:
            spr_read[spr] += 1
        elif xo10 == 467:
            spr_write[spr] += 1
        elif xo10 == 371:
            c['mftb'] += 1
        elif xo10 in (597, 725, 533, 661):
            c['lswi/stswi/lswx/stswx (string)'] += 1
        elif xo10 in (86, 54, 278, 246, 1014, 470, 982):
            c[{86: 'dcbf', 54: 'dcbst', 278: 'dcbt', 246: 'dcbtst', 1014: 'dcbz', 470: 'dcbi', 982: 'icbi'}[xo10]] += 1
        elif xo10 == 4:
            c['tw (trap)'] += 1
        elif xo10 in (20, 150):
            c['lwarx/stwcx.'] += 1
        elif xo10 in (83, 146, 210, 242, 595, 659):
            c['mfmsr/mtmsr/mtsr/mfsr (supervisor)'] += 1
        elif xo10 in (310, 438):
            c['eciwx/ecowx'] += 1
        elif xo10 == 598:
            c['sync'] += 1
        elif xo10 == 854:
            c['eieio'] += 1
    elif op == 4:
        xo5, xo6 = (w >> 1) & 31, (w >> 1) & 63
        if xo10 == 1014:
            c['dcbz_l (locked cache)'] += 1
        elif xo6 in (6, 7, 38, 39):
            ps['psq_lx/stx (indexed)'] += 1
        elif xo5 in (10, 11, 12, 13, 14, 15, 18, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31):
            ps[f'ps arith xo5={xo5}'] += 1
        else:
            ps[f'ps other xo10={xo10}'] += 1
    elif op in (56, 57, 60, 61):
        ps['psq_l/st'] += 1
    elif op in (46, 47):
        c['lmw/stmw'] += 1
    elif op == 3:
        c['twi (trap)'] += 1
    elif op == 17:
        c['sc (syscall)'] += 1
    elif op == 15 and (w & 0xFFFF) in (0xE000, 0xE001):
        lc_addr += 1

SPR_NAMES = {1: 'XER', 8: 'LR', 9: 'CTR', 26: 'SRR0', 27: 'SRR1', 22: 'DEC', 268: 'TBL', 269: 'TBU', 272: 'SPRG0',
             273: 'SPRG1', 274: 'SPRG2', 275: 'SPRG3', 284: 'TBL(w)', 285: 'TBU(w)', 287: 'PVR',
             912: 'GQR0', 913: 'GQR1', 914: 'GQR2', 915: 'GQR3', 916: 'GQR4', 917: 'GQR5', 918: 'GQR6',
             919: 'GQR7', 920: 'HID2', 921: 'WPAR', 922: 'DMAU', 923: 'DMAL', 1008: 'HID0', 1009: 'HID1',
             1017: 'L2CR', 1019: 'ICTC', 1020: 'THRM1', 1021: 'THRM2', 1022: 'THRM3', 936: 'UMMCR0',
             952: 'MMCR0', 953: 'PMC1', 954: 'PMC2', 956: 'MMCR1', 957: 'PMC3', 958: 'PMC4', 1013: 'DABR',
             1010: 'IABR', 944: 'UMMCR1'}
print(f"reached instructions: {c['total']}")
for k, v in sorted(c.items(), key=lambda x: -x[1]):
    if k != 'total':
        print(f'  {k}: {v}')
print(f'  lis 0xE000/0xE001 (locked cache addresses): {lc_addr}')
print('paired singles:')
for k, v in sorted(ps.items(), key=lambda x: -x[1]):
    print(f'  {k}: {v}')
print('mfspr:', ', '.join(f'{SPR_NAMES.get(s, s)}={n}' for s, n in spr_read.most_common()))
print('mtspr:', ', '.join(f'{SPR_NAMES.get(s, s)}={n}' for s, n in spr_write.most_common()))
