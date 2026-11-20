"""Prints a Wii DOL's sections and a few engine/SDK strings.
Usage: python3 dol-info.py main.dol"""
import re, struct, sys, collections

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
bss_addr, bss_size, entry = struct.unpack('>3I', data[216:228])
text = 0
for i in range(18):
    if sizes[i]:
        kind = 'text' if i < 7 else 'data'
        if kind == 'text':
            text += sizes[i]
        print(f'{kind}{i if i < 7 else i-7}: addr {addrs[i]:08x} size {sizes[i]:8x} ({sizes[i]/1024:.0f} KiB)')
print(f'bss {bss_addr:08x} size {bss_size:x} ({bss_size/1048576:.1f} MiB), entry {entry:08x}')
print(f'code total {text/1048576:.2f} MiB = ~{text//4} instructions')

# Count paired-single (opcode 4) and psq_l/psq_st (56,57,60,61) instructions in text.
ops = collections.Counter()
for i in range(7):
    if not sizes[i]:
        continue
    seg = data[offs[i]:offs[i]+sizes[i]]
    for (w,) in struct.iter_unpack('>I', seg):
        ops[w >> 26] += 1
print('primary op 4 (paired singles):', ops[4], ' psq_l/psq_st(u):', ops[56]+ops[57]+ops[60]+ops[61],
      ' fp (59/63):', ops[59]+ops[63])

strings = re.findall(rb'[\x20-\x7e]{6,}', data)
pat = re.compile(rb'(?i)(sdk|nw4r|alchemy|vicarious|toys ?for ?bob|igOpt|ig[A-Z]\w+|havok|bink|criware|fmod|wwise|lua|portal|\.cpp|\.c\b|build|version|copyright|\(c\)|__FILE__|RVL|<< ?RVL)')
hits = collections.OrderedDict()
for s in strings:
    if pat.search(s) and s not in hits:
        hits[s] = 1
for s in list(hits)[:200]:
    print(s.decode('ascii', 'replace'))
