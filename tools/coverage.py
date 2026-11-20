"""Reports how much of each code section dtk's symbols.txt covers with functions,
and lists the biggest uncovered gaps.
Usage: python3 coverage.py symbols.txt"""
import re, sys

SECTIONS = {'.init': (0x80004000, 0x2734), '.text': (0x80006920, 0x41C050), '.text2': (0x804CF8E0, 0xB87E0)}
rx = re.compile(r'^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)')
funcs = {s: [] for s in SECTIONS}
named = 0
total = 0
for line in open(sys.argv[1]):
    m = rx.match(line)
    if not m:
        continue
    name, addr, size = m.group(1), int(m.group(3), 16), int(m.group(4), 16)
    sec = next((k for k, (st, sz) in SECTIONS.items() if st <= addr < st + sz), None)
    if sec is None:
        continue
    funcs[sec].append((addr, size, name))
    total += 1
    if not name.startswith('fn_'):
        named += 1
print(f'functions: {total}, named (not fn_*): {named}')
for sec, (start, size) in SECTIONS.items():
    fs = sorted(funcs[sec])
    covered = sum(s for _, s, _ in fs)
    gaps = []
    pos = start
    for a, s, n in fs:
        if a > pos:
            gaps.append((a - pos, pos))
        pos = max(pos, a + s)
    if start + size > pos:
        gaps.append((start + size - pos, pos))
    print(f'{sec}: {len(fs)} functions, {covered/size*100:.2f}% covered, {len(gaps)} gaps')
    for g, a in sorted(gaps, reverse=True)[:5]:
        print(f'   gap 0x{a:08X} size 0x{g:X}')
sizes = sorted((s for v in funcs.values() for _, s, _ in v), reverse=True)
print('largest functions (bytes):', [hex(s) for s in sizes[:8]])
