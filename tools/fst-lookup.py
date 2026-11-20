"""Names the disc file at each partition-data offset (Wii FST: offsets << 2).
Usage: python3 fst-lookup.py <DATA dir> offset_hex..."""
import os, struct, sys

root = sys.argv[1]
boot = open(os.path.join(root, 'sys/boot.bin'), 'rb').read()
fst = open(os.path.join(root, 'sys/fst.bin'), 'rb').read()
dol = struct.unpack('>I', boot[0x420:0x424])[0] << 2
fsto = struct.unpack('>I', boot[0x424:0x428])[0] << 2
pieces = [(0, 0x440, 'sys/boot.bin'), (0x440, 0x2000, 'sys/bi2.bin'), (dol, os.path.getsize(os.path.join(root, 'sys/main.dol')), 'sys/main.dol'),
          (fsto, len(fst), 'sys/fst.bin')]
count = struct.unpack('>I', fst[8:12])[0]
names = fst[count * 12:]
dirs = [(count, '')]
for i in range(1, count):
    while len(dirs) > 1 and i >= dirs[-1][0]:
        dirs.pop()
    e = fst[i * 12:i * 12 + 12]
    is_dir = e[0] != 0
    name_off = struct.unpack('>I', e[0:4])[0] & 0xFFFFFF
    name = names[name_off:names.index(b'\0', name_off)].decode()
    a, b = struct.unpack('>II', e[4:12])
    path = dirs[-1][1] + '/' + name
    if is_dir:
        dirs.append((b, path))
    else:
        pieces.append((a << 2, b, 'files' + path))
for arg in sys.argv[2:]:
    off = int(arg, 16)
    hit = [p for p in pieces if p[0] <= off < p[0] + p[1]]
    print(f'{off:x}: ' + (f'{hit[0][2]} +0x{off - hit[0][0]:x} (size 0x{hit[0][1]:x})' if hit else 'no file'))
