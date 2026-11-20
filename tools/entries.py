"""Builds the recompiler's function entry list for a Wii DOL and measures how much code it reaches.

Entries = dtk's functions (symbols.txt)
        + code addresses stored in data sections (vtables, callback tables)
        + code addresses built with lis/addi in code (Alchemy meta-object registration)
        + 'stwu r1,-N(r1)' prologues that follow a function end (blr / b / padding)
        + every bl target reached from the above (repeated until nothing new).
Each entry is walked like the recompiler's boundary tracker: follow b/bc inside the function,
stop at blr/bctr/rfi, treat bl as a call (its target becomes an entry), and an unconditional b to
another entry as a tail call. Jump tables (dtk's jumptable_* objects) are followed for bctr.

Usage: python3 entries.py main.dol symbols.txt out_entries.txt"""
import re, struct, sys

dol_path, sym_path, out_path = sys.argv[1:4]
data = open(dol_path, 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
CODE = [(addrs[j], sizes[j], offs[j]) for j in range(7) if sizes[j]]
DATA = [(addrs[j], sizes[j], offs[j]) for j in range(7, 18) if sizes[j]]
NAMES = {0x80004000: '.init', 0x80006920: '.text', 0x804CF8E0: '.text2'}
# .init's functions start after the debugger's exception vector table (0x80004000-0x80005F34, copied
# to the vectors, never run in place): memcpy, memset, __start, __init_* are translated too
FIRST_CODE = 0x80005F34

def in_code(a):
    return a % 4 == 0 and any(s <= a < s + n for s, n, _ in CODE)

def word(a):
    for s, n, o in CODE + DATA:
        if s <= a < s + n:
            return struct.unpack('>I', data[o + a - s:o + a - s + 4])[0]
    return None

# --- dtk symbols: functions and jump tables
funcs, jumptables = set(), {}
rx = re.compile(r'^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?')
for line in open(sym_path):
    m = rx.match(line)
    if not m:
        continue
    a, kind, size = int(m.group(3), 16), m.group(4), int(m.group(5) or '0', 16)
    if kind == 'function' and a >= FIRST_CODE:
        funcs.add(a)
    elif m.group(1).startswith('jumptable_'):
        jumptables[a] = size
dtk_count = len(funcs)
entries = set(funcs)

# --- code pointers in data sections
data_ptrs = set()
for s, n, o in DATA:
    for (w,) in struct.iter_unpack('>I', data[o:o + n // 4 * 4]):
        if in_code(w) and w >= 0x80006920:
            data_ptrs.add(w)

# --- lis/addi code addresses and prologues
lis_ptrs, prologues = set(), set()
for s, n, o in CODE:
    hi = {}
    for k, (w,) in enumerate(struct.iter_unpack('>I', data[o:o + n])):
        a = s + 4 * k
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:
            hi[rd] = imm << 16
        elif op == 14 and ra and ra in hi:
            t = (hi[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            if in_code(t) and t >= 0x80006920:
                lis_ptrs.add(t)
        if (w & 0xFFFF8000) == 0x94218000 and a >= 0x80006920:
            p = word(a - 4)
            if p is None or p in (0x4E800020, 0x4C000064, 0) or ((p >> 26) == 18 and (p & 1) == 0):
                prologues.add(a)
        if op in (18, 19):
            hi.clear()

# Jump table targets are labels inside functions, not entries.
jt_targets = set()
for a, size in jumptables.items():
    for i in range(0, size, 4):
        t = word(a + i)
        if t and in_code(t):
            jt_targets.add(t)

# Addresses from data/lis that are jump-table labels are not functions.
data_ptrs -= jt_targets
lis_ptrs -= jt_targets
entries |= data_ptrs | lis_ptrs | prologues

def walk(entry, all_entries):
    """Returns (instruction addresses of the function, call targets, problems)."""
    seen, todo, calls, probs = set(), [entry], set(), []
    while todo:
        a = todo.pop()
        while True:
            if a in seen:
                break
            if not in_code(a):
                probs.append(f'{entry:08X}: runs out of code at {a:08X}')
                break
            if a != entry and a in all_entries and a not in seen and seen:
                # Fell through or branched into another known entry: treat as a tail call.
                break
            seen.add(a)
            w = word(a)
            op = w >> 26
            if op == 18:                                  # b / bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = li if w & 2 else a + li
                if w & 1:
                    calls.add(t)
                    a += 4
                    continue
                if t in all_entries and t != entry:       # tail call
                    calls.add(t)
                    break
                a = t
                continue
            if op == 16:                                  # bc
                bd = w & 0xFFFC
                if bd & 0x8000:
                    bd -= 0x10000
                t = bd if w & 2 else a + bd
                if w & 1:
                    calls.add(t)
                else:
                    todo.append(t)
                bo = (w >> 21) & 31
                if (bo & 0x14) == 0x14:                    # branch always
                    if w & 1:
                        a += 4
                        continue
                    break
                a += 4
                continue
            if op == 19:
                xo = (w >> 1) & 0x3FF
                bo = (w >> 21) & 31
                if xo == 16:                              # bclr
                    if w & 1:
                        a += 4
                        continue
                    if (bo & 0x14) == 0x14:
                        break
                    a += 4
                    continue
                if xo == 528:                             # bcctr: jump table or indirect tail call
                    if w & 1:
                        a += 4
                        continue
                    for jt, size in jumptables.items():
                        pass
                    if (bo & 0x14) == 0x14:
                        break
                    a += 4
                    continue
                if xo == 50:                              # rfi
                    break
            if w == 0:
                probs.append(f'{entry:08X}: zero word at {a:08X}')
                break
            a += 4
    return seen, calls, probs

# Jump table targets get walked as part of whichever function reaches them; to keep it simple,
# walk them as extra roots of the function that contains the jump table's bctr (approximated by
# walking them on their own and counting their bytes as covered).
covered, problems = set(), []
pending = sorted(entries)
done = set()
while pending:
    e = pending.pop()
    if e in done:
        continue
    done.add(e)
    body, calls, probs = walk(e, entries)
    covered |= body
    problems += probs
    for c in calls:
        if in_code(c) and c >= FIRST_CODE and c not in entries:
            entries.add(c)
            pending.append(c)
for t in jt_targets:
    body, calls, probs = walk(t, entries | {t})
    covered |= body

print(f'entries: {len(entries)} (dtk {dtk_count}, data pointers {len(data_ptrs)}, lis/addi {len(lis_ptrs)}, '
      f'prologues {len(prologues)}, jump table labels {len(jt_targets)})')
for s, n, _ in CODE:
    if s < 0x80006920:
        continue
    inside = sum(1 for a in covered if s <= a < s + n) * 4
    zeros = sum(1 for a in range(s, s + n, 4) if word(a) == 0) * 4
    print(f'{NAMES.get(s, hex(s))}: reached {inside / n * 100:.2f}% of bytes '
          f'({(n - inside - zeros) / 1024:.1f} KiB unreached, excluding {zeros/1024:.1f} KiB zero padding)')
import collections
src = collections.Counter()
bad = set()
for p in problems:
    e = int(p[:8], 16)
    bad.add(e)
for e in bad:
    tag = 'dtk' if e in funcs else 'data' if e in data_ptrs else 'lis' if e in lis_ptrs else 'prologue' if e in prologues else 'bl-target'
    src[tag] += 1
print(f'problems: {len(problems)} in {len(bad)} entries, by source: {dict(src)}')
dtk_bad = sorted(e for e in bad if e in funcs)
print('dtk entries with problems:', [f'{e:08X}' for e in dtk_bad[:10]])
# Entries that are not dtk functions and whose walk hits garbage are false: leave them out.
entries -= {e for e in bad if e not in funcs}
# Data inside .text2 (found 2026-11-14): Alchemy's embedded XML config text, and an embedded
# 'IGA\x1a' archive. ASCII and archive bytes decode as valid PowerPC, so the walk can't tell.
DATA_IN_CODE = [(0x804CFC46, 0x804D0D08), (0x804D2306, 0x804D2490), (0x804D2598, 0x804E9604)]
in_data = {e for e in entries if any(s <= e < t for s, t in DATA_IN_CODE)}
# Pointer tables in code (e.g. 0x80555870: eight words like 0x8012EFA4): an address constant (not a
# dtk function, prologue or call target) whose first 4 words are all MEM1 addresses is data.
def pointer_table(e):
    ws = [word(e + 4 * i) for i in range(4)]
    return all(w is not None and 0x80000000 <= w < 0x81800000 for w in ws)
# Only counted, not dropped: four lwz r0-r3 in a row look the same, and a real function missing from
# the list costs more (interpreted at runtime) than a table compiled as dead code.
tables = {e for e in entries if e not in funcs and e not in prologues and e in (lis_ptrs | data_ptrs) and pointer_table(e)}
print(f'possible pointer tables (kept): {len(tables)}')
entries -= in_data
print(f'entries after dropping false ones: {len(entries)} ({len(in_data)} in data inside code)')
for p in problems[:15]:
    print('  ', p)
with open(out_path, 'w') as f:
    for e in sorted(entries):
        f.write(f'{e:08X}\n')
