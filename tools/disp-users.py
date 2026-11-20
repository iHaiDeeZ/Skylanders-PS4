"""Lists the D-form loads/stores that use a given displacement (a struct field offset), with the
function each one is in (dtk symbols).
Usage: python3 disp-users.py main.dol symbols.txt displacement_hex [stores]"""
import bisect, re, struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
disp = int(sys.argv[3], 16)
stores_only = len(sys.argv) > 4

funcs = []
for line in open(sys.argv[2]):
    m = re.match(r'^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function', line)
    if m:
        funcs.append((int(m.group(2), 16), m.group(1)))
funcs.sort()
starts = [f[0] for f in funcs]

NAMES = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu', 38: 'stb', 39: 'stbu', 40: 'lhz', 41: 'lhzu',
         42: 'lha', 43: 'lhau', 44: 'sth', 45: 'sthu', 48: 'lfs', 50: 'lfd', 52: 'stfs', 54: 'stfd', 14: 'addi'}
STORES = {36, 37, 38, 39, 44, 45, 52, 54}
for j in range(7):  # text sections
    if not sizes[j]:
        continue
    for k in range(0, sizes[j], 4):
        w = struct.unpack('>I', data[offs[j] + k:offs[j] + k + 4])[0]
        op = w >> 26
        if op not in NAMES or (w & 0xFFFF) != disp:
            continue
        if stores_only and op not in STORES:
            continue
        a = addrs[j] + k
        i = bisect.bisect_right(starts, a) - 1
        name = f'{funcs[i][1]}+0x{a - funcs[i][0]:x}' if i >= 0 else '?'
        print(f'{a:08X} {NAMES[op]} r{(w >> 21) & 31}, 0x{disp:x}(r{(w >> 16) & 31})  {name}')
