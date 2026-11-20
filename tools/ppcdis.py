"""Disassembles a range of a Wii DOL (Gekko: paired singles shown as .long by capstone).
Usage: python3 dis.py main.dol start_hex count"""
import struct, sys
import capstone

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack('>18I', data[0:72])
addrs = struct.unpack('>18I', data[72:144])
sizes = struct.unpack('>18I', data[144:216])
start, count = int(sys.argv[2], 16), int(sys.argv[3])
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
for j in range(18):
    if sizes[j] and addrs[j] <= start < addrs[j] + sizes[j]:
        o = offs[j] + start - addrs[j]
        for k in range(count):
            a = start + 4 * k
            b = data[o + 4 * k:o + 4 * k + 4]
            ins = list(md.disasm(b, a))
            text = f'{ins[0].mnemonic} {ins[0].op_str}' if ins else f'.long 0x{b.hex()}'
            print(f'{a:08X}: {b.hex()}  {text}')
        break
