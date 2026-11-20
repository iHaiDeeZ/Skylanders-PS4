"""Adds dtk function names to every 8-digit hex address in text read from stdin
(80383E2C -> 80383E2C(OSDisableInterrupts+0x0)).
Usage: ... | python3 addr2name.py symbols.txt"""
import bisect, re, sys

funcs = []
for line in open(sys.argv[1]):
    m = re.match(r'^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function(?: size:0x([0-9A-Fa-f]+))?', line)
    if m:
        funcs.append((int(m.group(2), 16), int(m.group(3) or '0', 16), m.group(1)))
funcs.sort()
starts = [f[0] for f in funcs]

def name(match):
    a = int(match.group(0), 16)
    if not 0x80004000 <= a < 0x81800000:
        return match.group(0)
    i = bisect.bisect_right(starts, a) - 1
    if i < 0 or (funcs[i][1] and a >= funcs[i][0] + funcs[i][1]):
        return match.group(0)
    return f'{match.group(0)}({funcs[i][2]}+0x{a - funcs[i][0]:x})'

for line in sys.stdin:
    sys.stdout.write(re.sub(r'\b[0-9a-fA-F]{8}\b', name, line))
