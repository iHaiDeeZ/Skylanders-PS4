"""Lists the bl call sites of each given function address (direct calls), with dtk names.
Usage: python3 callers.py main.dol address_hex...  (pipe through addr2name.py for names)"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
targets = {int(a, 16) for a in sys.argv[2:]}
for j in range(7):
    if not sizes[j]:
        continue
    seg = data[offs[j]:offs[j] + sizes[j]]
    for k, (w,) in enumerate(struct.iter_unpack('>I', seg)):
        if (w >> 26) == 18 and (w & 1):
            li = w & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            a = addrs[j] + 4 * k
            t = li if w & 2 else a + li
            if t in targets:
                print(f'{t:08X} called from {a:08X}')
