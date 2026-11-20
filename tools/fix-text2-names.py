"""Renames the DOL's second '.text' section to '.text2' in dtk's splits.txt and symbols.txt.
Usage: python3 fix-text2-names.py splits.txt symbols.txt"""
import re, sys
T2_START, T2_END = 0x804CF8E0, 0x804CF8E0 + 0xB87E0
splits, syms = sys.argv[1], sys.argv[2]
lines = open(splits).read().split('\n')
seen = 0
for i, l in enumerate(lines):
    if re.match(r'^\t\.text\s', l):
        seen += 1
        if seen == 2:
            lines[i] = l.replace('.text ', '.text2', 1)
open(splits, 'w').write('\n'.join(lines))
out = []
for l in open(syms):
    m = re.match(r'^(\S+ = )\.text:0x([0-9A-Fa-f]+)(;.*)$', l.rstrip('\n'))
    if m and T2_START <= int(m.group(2), 16) < T2_END:
        l = f'{m.group(1)}.text2:0x{m.group(2)}{m.group(3)}\n'
    out.append(l)
open(syms, 'w').write(''.join(out))
print('renamed sections seen:', seen)
