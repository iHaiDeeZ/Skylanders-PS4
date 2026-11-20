"""Writes a Wii DOL's code sections as jittest JT_RPX records: {u32 address, u32 size} little-endian
header followed by the code bytes as is (big-endian), one record per text section.
Usage: python3 dol2bin.py main.dol out.bin"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
with open(sys.argv[2], 'wb') as out:
    for j in range(7):
        if not sizes[j]:
            continue
        out.write(struct.pack('<II', addrs[j], sizes[j]) + data[offs[j]:offs[j] + sizes[j]])
        print(f'code section at {addrs[j]:#x}, {sizes[j]:#x} bytes')
