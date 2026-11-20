"""Finds function entry points dtk missed and appends them to symbols.txt as seeds.

Sources:
  1. lis rX,hi / addi rY,rX,lo pairs anywhere in code whose target is in a code section
     (Alchemy registers meta-object callbacks this way).
  2. 'stwu r1,-N(r1)' prologues that are not inside a known function.
Only addresses outside every known function are added, so existing analysis is not overridden.

Usage: python3 seed-functions.py main.dol symbols.txt   (prints how many seeds were added)"""
import bisect, re, struct, sys

dol_path, sym_path = sys.argv[1], sys.argv[2]
data = open(dol_path, 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
code = [(addrs[j], sizes[j], offs[j]) for j in range(7) if sizes[j]]

def in_code(a):
    return a % 4 == 0 and any(s <= a < s + n for s, n, _ in code)

rx = re.compile(r'^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:function(?: size:0x([0-9A-Fa-f]+))?')
known = []
for line in open(sym_path):
    m = rx.match(line)
    if m:
        known.append((int(m.group(3), 16), int(m.group(4) or '4', 16)))
known.sort()
starts = [a for a, _ in known]
known_set = set(starts)

def inside_known(a):
    k = bisect.bisect_right(starts, a) - 1
    return k >= 0 and starts[k] <= a < starts[k] + known[k][1]

cands = set()
for s, n, o in code:
    hi = {}
    for k, (w,) in enumerate(struct.iter_unpack('>I', data[o:o + n])):
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:                       # lis
            hi[rd] = imm << 16
            continue
        if op == 14 and ra != 0 and ra in hi:           # addi
            simm = imm - 0x10000 if imm & 0x8000 else imm
            t = (hi[ra] + simm) & 0xFFFFFFFF
            if in_code(t):
                cands.add(t)
        if op == 24 and ra in hi:                       # ori (rS=rd field, rA=ra field swapped)
            pass
        if (w & 0xFFFF8000) == 0x94218000:              # stwu r1,-N(r1)
            cands.add(s + 4 * k)
        if op in (18, 19):                              # branches end lis tracking for safety
            hi.clear()

def word(a):
    for s, n, o in code:
        if s <= a < s + n:
            return struct.unpack('>I', data[o + a - s:o + a - s + 4])[0]
    return None

def after_function_end(a):
    """True when the previous instruction ends a function: blr, unconditional b, rfi, or zero padding."""
    w = word(a - 4)
    if w is None:
        return True
    return w in (0x4E800020, 0x4C000064, 0) or ((w >> 26) == 18 and (w & 1) == 0)

new = sorted(a for a in cands
             if a >= 0x80006920 and a not in known_set and not inside_known(a) and after_function_end(a))
with open(sym_path, 'a') as f:
    for a in new:
        sec = '.init' if a < 0x80006920 else ('.text2' if 0x804CF8E0 <= a < 0x804CF8E0 + 0xB87E0 else '.text')
        f.write(f'fn_{a:08X} = {sec}:0x{a:08X}; // type:function\n')
print(f'candidates {len(cands)}, added {len(new)} seeds')
