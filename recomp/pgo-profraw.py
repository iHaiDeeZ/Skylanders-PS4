"""A .profraw (LLVM's raw profile, format 10: LLVM 19-21) from an instrumented translated code file and
the counters the run left in its image.

The translated code file (aotbuild.sh with AOT_CFLAGS="-fprofile-generate -mllvm -disable-vp=true")
can't hold LLVM's profile runtime: it imports nothing. So the run (ssa_run.cpp SSA_AOT_DUMP) writes
only the counters section from memory, and this writes the file the runtime would have: the header,
the per-function records (__llvm_prf_data), the counters, the names (__llvm_prf_names). The records
point at their counters relative to themselves, so the file's own section addresses serve.

  pgo-profraw.py <instrumented .aot> <counters dump> <out.profraw>
  pgo-profraw.py <instrumented .aot> --where        (the dump's place: "<cnts address> <size>")
"""
import struct
import sys

MAGIC = (255 << 56 | ord('l') << 48 | ord('p') << 40 | ord('r') << 32 | ord('o') << 24 | ord('f') << 16 | ord('r') << 8 | 129)
DATA_RECORD = 64  # sizeof(__llvm_profile_data): 6 x 8 + 4 + 3 x 2 + 4, 8-aligned
IPVK_LAST = 2     # IPVK_VTableTarget


def sections(elf):
    """name -> (address, file offset, size, index); the symbols: name -> (value, section index)"""
    if elf[:4] != b'\x7fELF' or elf[4] != 2 or elf[5] != 1:
        sys.exit('not a 64-bit little-endian ELF file')
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x3A)
    heads = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('<IIQQQQIIQQ', elf, shoff + i * shentsize)
        heads.append((name, typ, addr, off, size, link, entsize))
    strtab = heads[shstrndx]
    def sname(n, tab_off):
        end = elf.index(b'\0', tab_off + n)
        return elf[tab_off + n:end].decode()
    out = {}
    syms = {}
    for i, (name, typ, addr, off, size, link, entsize) in enumerate(heads):
        out[sname(name, strtab[3])] = (addr, off, size, i)
        if typ == 2:  # SHT_SYMTAB
            str_off = heads[link][3]
            for k in range(size // 24):
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', elf, off + k * 24)
                if st_name:
                    syms[sname(st_name, str_off)] = (st_value, st_shndx)
    return out, syms, heads


def pad8(n):
    return (8 - n % 8) % 8


def main():
    if len(sys.argv) == 3 and sys.argv[2] == '--where':
        elf = open(sys.argv[1], 'rb').read()
        secs, syms, heads = sections(elf)
        addr, off, size, idx = secs['__llvm_prf_cnts']
        print(addr, size)
        return
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf = open(sys.argv[1], 'rb').read()
    counters = open(sys.argv[2], 'rb').read()
    secs, syms, heads = sections(elf)
    for need in ('__llvm_prf_data', '__llvm_prf_cnts', '__llvm_prf_names'):
        if need not in secs:
            sys.exit(f'no {need} section: not an instrumented build')
    d_addr, d_off, d_size, _ = secs['__llvm_prf_data']
    c_addr, c_off, c_size, _ = secs['__llvm_prf_cnts']
    n_addr, n_off, n_size, _ = secs['__llvm_prf_names']
    b_addr, b_off, b_size, _ = secs.get('__llvm_prf_bits', (0, 0, 0, 0))
    if d_size % DATA_RECORD:
        sys.exit(f'__llvm_prf_data is {d_size} bytes: not whole {DATA_RECORD}-byte records')
    if len(counters) != c_size:
        sys.exit(f'the dump is {len(counters)} bytes, __llvm_prf_cnts {c_size}')
    if b_size:
        sys.exit('bitmap bytes (MC/DC): not handled')
    # the raw version the instrumentation was made for (its variant bits: IR-level and so on)
    if '__llvm_profile_raw_version' not in syms:
        sys.exit('no __llvm_profile_raw_version')
    v_value, v_shndx = syms['__llvm_profile_raw_version']
    _, _, v_secaddr, v_secoff, _, _, _ = heads[v_shndx]
    version, = struct.unpack_from('<Q', elf, v_secoff + v_value - v_secaddr)
    if version & 0xFFFFFFFF != 10:
        sys.exit(f'raw profile version {version & 0xFFFFFFFF}, this writes 10')
    mask = (1 << 64) - 1
    header = struct.pack('<16Q',
                         MAGIC,
                         version,
                         0,                         # BinaryIdsSize
                         d_size // DATA_RECORD,     # NumData
                         0,                         # PaddingBytesBeforeCounters
                         c_size // 8,               # NumCounters
                         pad8(c_size),              # PaddingBytesAfterCounters
                         0,                         # NumBitmapBytes
                         0,                         # PaddingBytesAfterBitmapBytes
                         n_size,                    # NamesSize
                         (c_addr - d_addr) & mask,  # CountersDelta
                         (0 - d_addr) & mask,       # BitmapDelta (no bitmap section)
                         n_addr,                    # NamesDelta
                         0,                         # NumVTables
                         0,                         # VNamesSize
                         IPVK_LAST)                 # ValueKindLast
    body = (elf[d_off:d_off + d_size] + counters + b'\0' * pad8(c_size) +
            elf[n_off:n_off + n_size] + b'\0' * pad8(n_size))
    open(sys.argv[3], 'wb').write(header + body)
    nonzero = sum(1 for k in range(0, c_size, 8) if counters[k:k + 8] != b'\0' * 8)
    print(f'{sys.argv[3]}: {d_size // DATA_RECORD} functions, {c_size // 8} counters ({nonzero} not zero), '
          f'names {n_size} bytes, version {version:#x}')


if __name__ == '__main__':
    main()
