"""Hardware registers without traps (format version 6): translated code's loads and stores in the
Wii's hardware register area (guest 0xCC000000-0xCDFFFFFF) call the runtime through two new imports
(hwRead, hwWrite) instead of touching guest memory there, where they fault (the runtime then decodes
the host instruction and emulates it: ~38 us a time on the PS4, where a busy screen makes tens of
thousands of them a second, mostly GX FIFO writes).

  patch-aot-hw.py crema <crema>/src   BackendC's prelude (new translations), AotFormat.h's version,
                                      the loader's imports (PPCRecompiler.cpp: g_cremaAotHwRead/Write)
  patch-aot-hw.py aot <dir>           C++ already generated (aot_*.cpp, aot_manifest.cpp): the same
                                      prelude and version, so no new translation run is needed

Idempotent (a patched file is left as it is)."""
import glob
import sys

MARK = 'hwRead'

OLD_IMPORTS_END = '\tuint32 (*leave)(uint8* h, uint32 at, uint32 lr0, uint32 sp0);\n};\n'
NEW_IMPORTS_END = ('\tuint32 (*leave)(uint8* h, uint32 at, uint32 lr0, uint32 sp0);\n'
    '\t// (6) the Wii\'s hardware registers (guest 0xCC000000-0xCDFFFFFF): loads and stores there call\n'
    '\t// these (the guest address, the size in bytes, the value as the guest sees it) instead of\n'
    '\t// touching guest memory, where they would trap\n'
    '\tuint32 (*hwRead)(uint32 address, uint32 size);\n'
    '\tvoid (*hwWrite)(uint32 address, uint32 value, uint32 size);\n'
    '};\n')

OLD_HELPERS = ('static inline uint32 aot_ld32(const uint8* p) { return *(const volatile uint32*)p; }\n'
    'static inline uint16 aot_ld16(const uint8* p) { return *(const volatile uint16*)p; }\n'
    'static inline uint8 aot_ld8(const uint8* p) { return *(const volatile uint8*)p; }\n'
    'static inline uint64 aot_ld64(const uint8* p) { return *(const volatile uint64*)p; }\n'
    'static inline void aot_st32(uint8* p, uint32 v) { *(volatile uint32*)p = v; }\n'
    'static inline void aot_st16(uint8* p, uint16 v) { *(volatile uint16*)p = v; }\n'
    'static inline void aot_st8(uint8* p, uint8 v) { *(volatile uint8*)p = v; }\n'
    'static inline void aot_st64(uint8* p, uint64 v) { *(volatile uint64*)p = v; }\n')

NEW_HELPERS = ('// (6) memory accesses: p = m + guest address (m: the translated function\'s memory base); the\n'
    '// hardware register area goes through the imports (out of line: the rare path kept small), values\n'
    '// in memory byte order here\n'
    '#define AOT_HW(p) __builtin_expect((uint64)((p) - m) - 0xCC000000ull < 0x02000000ull, 0)\n'
    '#define AOT_COLD __attribute__((noinline, cold))\n'
    'AOT_COLD static uint32 aot_hw_ld32(uint32 a) { return __builtin_bswap32(aot_imports.hwRead(a, 4)); }\n'
    'AOT_COLD static uint16 aot_hw_ld16(uint32 a) { return __builtin_bswap16((uint16)aot_imports.hwRead(a, 2)); }\n'
    'AOT_COLD static uint8 aot_hw_ld8(uint32 a) { return (uint8)aot_imports.hwRead(a, 1); }\n'
    'AOT_COLD static uint64 aot_hw_ld64(uint32 a) { const uint64 hi = aot_imports.hwRead(a, 4), lo = aot_imports.hwRead(a + 4, 4); return __builtin_bswap64(hi << 32 | lo); }\n'
    'AOT_COLD static void aot_hw_st32(uint32 a, uint32 v) { aot_imports.hwWrite(a, __builtin_bswap32(v), 4); }\n'
    'AOT_COLD static void aot_hw_st16(uint32 a, uint16 v) { aot_imports.hwWrite(a, __builtin_bswap16(v), 2); }\n'
    'AOT_COLD static void aot_hw_st8(uint32 a, uint8 v) { aot_imports.hwWrite(a, v, 1); }\n'
    'AOT_COLD static void aot_hw_st64(uint32 a, uint64 v) { const uint64 g = __builtin_bswap64(v); aot_imports.hwWrite(a, (uint32)(g >> 32), 4); aot_imports.hwWrite(a + 4, (uint32)g, 4); }\n'
    'static inline uint32 aot_ld32_(const uint8* m, const uint8* p) { if (AOT_HW(p)) return aot_hw_ld32((uint32)(p - m)); return *(const volatile uint32*)p; }\n'
    'static inline uint16 aot_ld16_(const uint8* m, const uint8* p) { if (AOT_HW(p)) return aot_hw_ld16((uint32)(p - m)); return *(const volatile uint16*)p; }\n'
    'static inline uint8 aot_ld8_(const uint8* m, const uint8* p) { if (AOT_HW(p)) return aot_hw_ld8((uint32)(p - m)); return *(const volatile uint8*)p; }\n'
    'static inline uint64 aot_ld64_(const uint8* m, const uint8* p) { if (AOT_HW(p)) return aot_hw_ld64((uint32)(p - m)); return *(const volatile uint64*)p; }\n'
    'static inline void aot_st32_(const uint8* m, uint8* p, uint32 v) { if (AOT_HW(p)) aot_hw_st32((uint32)(p - m), v); else *(volatile uint32*)p = v; }\n'
    'static inline void aot_st16_(const uint8* m, uint8* p, uint16 v) { if (AOT_HW(p)) aot_hw_st16((uint32)(p - m), v); else *(volatile uint16*)p = v; }\n'
    'static inline void aot_st8_(const uint8* m, uint8* p, uint8 v) { if (AOT_HW(p)) aot_hw_st8((uint32)(p - m), v); else *(volatile uint8*)p = v; }\n'
    'static inline void aot_st64_(const uint8* m, uint8* p, uint64 v) { if (AOT_HW(p)) aot_hw_st64((uint32)(p - m), v); else *(volatile uint64*)p = v; }\n'
    '// the check only where AOT_HWCHECK is 1 when a function is compiled (patch-aot-hw.py select: the\n'
    '// functions that load a hardware address; elsewhere a hardware access traps, as before)\n'
    '#ifndef AOT_HWCHECK\n#define AOT_HWCHECK 1\n#endif\n'
    'static inline uint32 aot_ld32p(const uint8* p) { return *(const volatile uint32*)p; }\n'
    'static inline uint16 aot_ld16p(const uint8* p) { return *(const volatile uint16*)p; }\n'
    'static inline uint8 aot_ld8p(const uint8* p) { return *(const volatile uint8*)p; }\n'
    'static inline uint64 aot_ld64p(const uint8* p) { return *(const volatile uint64*)p; }\n'
    'static inline void aot_st32p(uint8* p, uint32 v) { *(volatile uint32*)p = v; }\n'
    'static inline void aot_st16p(uint8* p, uint16 v) { *(volatile uint16*)p = v; }\n'
    'static inline void aot_st8p(uint8* p, uint8 v) { *(volatile uint8*)p = v; }\n'
    'static inline void aot_st64p(uint8* p, uint64 v) { *(volatile uint64*)p = v; }\n'
    '#define aot_ld32(p) (AOT_HWCHECK ? aot_ld32_(m, (p)) : aot_ld32p(p))\n'
    '#define aot_ld16(p) (AOT_HWCHECK ? aot_ld16_(m, (p)) : aot_ld16p(p))\n'
    '#define aot_ld8(p) (AOT_HWCHECK ? aot_ld8_(m, (p)) : aot_ld8p(p))\n'
    '#define aot_ld64(p) (AOT_HWCHECK ? aot_ld64_(m, (p)) : aot_ld64p(p))\n'
    '#define aot_st32(p, v) (AOT_HWCHECK ? aot_st32_(m, (p), (v)) : aot_st32p((p), (v)))\n'
    '#define aot_st16(p, v) (AOT_HWCHECK ? aot_st16_(m, (p), (v)) : aot_st16p((p), (v)))\n'
    '#define aot_st8(p, v) (AOT_HWCHECK ? aot_st8_(m, (p), (v)) : aot_st8p((p), (v)))\n'
    '#define aot_st64(p, v) (AOT_HWCHECK ? aot_st64_(m, (p), (v)) : aot_st64p((p), (v)))\n')


def sub_once(text, old, new, what, path):
    if old not in text:
        sys.exit(f'patch-aot-hw: not found ({what}) in {path}')
    return text.replace(old, new, 1)


def patch_prelude(text, path):
    text = sub_once(text, OLD_IMPORTS_END, NEW_IMPORTS_END, 'imports end', path)
    return sub_once(text, OLD_HELPERS, NEW_HELPERS, 'memory helpers', path)


def rw(path, fn):
    s = open(path, encoding='utf-8', newline='').read()
    if MARK in s:
        return False
    s = fn(s, path)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    return True


def crema(src):
    rec = f'{src}/Cafe/HW/Espresso/Recompiler'
    n = 0
    n += rw(f'{rec}/BackendC/BackendC.cpp', patch_prelude)
    def fmt(s, p):
        s = sub_once(s, 'constexpr uint32 kCremaAotVersion = 5;',
            '// 6: hardware register accesses through the imports\' hwRead/hwWrite (patch-aot-hw.py)\n'
            'constexpr uint32 kCremaAotVersion = 6; // (hwRead)', 'version', p)
        return s
    n += rw(f'{rec}/BackendC/AotFormat.h', fmt)
    def loader(s, p):
        s = sub_once(s, '\t\tuint32 (*leave)(uint8* h, uint32 at, uint32 lr0, uint32 sp0); // CremaAotLeave\n\t};\n',
            '\t\tuint32 (*leave)(uint8* h, uint32 at, uint32 lr0, uint32 sp0); // CremaAotLeave\n'
            '\t\tuint32 (*hwRead)(uint32 address, uint32 size); // g_cremaAotHwRead (patch-aot-hw.py)\n'
            '\t\tvoid (*hwWrite)(uint32 address, uint32 value, uint32 size);\n\t};\n', 'loader imports', p)
        s = sub_once(s, '\t\timports.leave = CremaAotLeave;\n',
            '\t\timports.leave = CremaAotLeave;\n'
            '\t\timports.hwRead = g_cremaAotHwRead ? g_cremaAotHwRead : CremaAotHwReadMemory;\n'
            '\t\timports.hwWrite = g_cremaAotHwWrite ? g_cremaAotHwWrite : CremaAotHwWriteMemory;\n', 'fill imports', p)
        s = sub_once(s, '\t// stubs: where the entries\' stubs are (nullptr: none, the functions never call each other)\n'
            '\tvoid CremaAotFillImports(',
            '\t// hardware register accesses from translated code without a runtime that takes them: plain\n'
            '\t// memory accesses (big-endian), as before\n'
            '\tuint32 CremaAotHwReadMemory(uint32 address, uint32 size)\n\t{\n'
            '\t\tconst uint8* p = memory_base + address;\n'
            '\t\treturn size == 4 ? _swapEndianU32(*(volatile uint32*)p) : size == 2 ? _swapEndianU16(*(volatile uint16*)p) : *(volatile uint8*)p;\n'
            '\t}\n'
            '\tvoid CremaAotHwWriteMemory(uint32 address, uint32 value, uint32 size)\n\t{\n'
            '\t\tuint8* p = memory_base + address;\n'
            '\t\tif (size == 4)\n\t\t\t*(volatile uint32*)p = _swapEndianU32(value);\n'
            '\t\telse if (size == 2)\n\t\t\t*(volatile uint16*)p = _swapEndianU16((uint16)value);\n'
            '\t\telse\n\t\t\t*(volatile uint8*)p = (uint8)value;\n'
            '\t}\n\n'
            '\t// stubs: where the entries\' stubs are (nullptr: none, the functions never call each other)\n'
            '\tvoid CremaAotFillImports(', 'fallbacks', p)
        s = sub_once(s, '\t// what the generated code calls and reads (BackendC.cpp\'s prelude: AotImports)\n',
            '}\n// the runtime\'s hardware register emulation for translated code (set before the load; null:\n'
            '// plain memory accesses)\nuint32 (*g_cremaAotHwRead)(uint32 address, uint32 size) = nullptr;\n'
            'void (*g_cremaAotHwWrite)(uint32 address, uint32 value, uint32 size) = nullptr;\nnamespace\n{\n'
            '\t// what the generated code calls and reads (BackendC.cpp\'s prelude: AotImports)\n', 'globals', p)
        return s
    n += rw(f'{rec}/PPCRecompiler.cpp', loader)
    print(f'crema: {n} files patched')


def aot(d):
    n = 0
    for path in sorted(glob.glob(f'{d}/aot_*.cpp')):
        def gen(s, p):
            s = patch_prelude(s, p)
            if p.endswith('aot_manifest.cpp'):
                s = sub_once(s, "'O', 'T', '0', '1'}, 5u,", "'O', 'T', '0', '1'}, 6u,", 'manifest version', p)
            return s
        n += rw(path, gen)
    print(f'aot: {n} files patched')


HW_HIGH = (0xCC00, 0xCC01, 0xCD00, 0xCD01, 0xCD80, 0xCD81)


def select(d, dol, extra=None):
    """Only the translated functions whose guest range has a lis rX,<hardware high half> keep the
    check (AOT_HWCHECK 1); the rest compile to plain accesses (the code stays as small as before)."""
    import re
    import struct
    data = open(dol, 'rb').read()
    offs = struct.unpack('>18I', data[0:72])
    addrs = struct.unpack('>18I', data[72:144])
    sizes = struct.unpack('>18I', data[144:216])
    lis = []
    for j in range(7):
        s, n, o = addrs[j], sizes[j], offs[j]
        for k, (w,) in enumerate(struct.iter_unpack('>I', data[o:o + n])):
            if w >> 26 == 15 and (w >> 16) & 31 == 0 and (w & 0xFFFF) in HW_HIGH:
                lis.append(s + 4 * k)
    lis.sort()
    man = open(f'{d}/aot_manifest.cpp', encoding='utf-8', newline='').read()
    recs = re.findall(r'\{0x([0-9a-f]+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u,', man)
    import bisect
    marked = set()
    for entry, start, length in recs:
        a, b = int(start, 16), int(start, 16) + int(length, 16)
        i = bisect.bisect_left(lis, a)
        if i < len(lis) and lis[i] < b:
            marked.add(int(entry, 16))
    # functions seen trapping in a run (ssa.log's "mmio: trapped in <entry> <count>" lines, SSA_MMIO_HIST=1):
    # hardware pointers that come from memory or arguments, which the lis scan can't see
    if extra:
        known = {int(e, 16) for e, _, _ in recs}
        for l in open(extra):
            w = l.split()
            if len(w) >= 4 and w[:3] == ['mmio:', 'trapped', 'in'] and int(w[3], 16) in known:
                marked.add(int(w[3], 16))
    print(f'select: {len(lis)} hardware lis, {len(marked)} of {len(recs)} functions keep the check')
    defn = re.compile(r'^AOT_FN uint32 aot_([0-9a-f]{8})\(uint8\* __restrict h', re.M)
    n = 0
    for path in sorted(glob.glob(f'{d}/aot_*.cpp')):
        s = open(path, encoding='utf-8', newline='').read()
        if '#undef AOT_HWCHECK' in s:
            continue
        def put(mo):
            on = 1 if int(mo.group(1), 16) in marked else 0
            return f'#undef AOT_HWCHECK\n#define AOT_HWCHECK {on}\n' + mo.group(0)
        s2 = defn.sub(put, s)
        if s2 != s:
            open(path, 'w', encoding='utf-8', newline='').write(s2)
            n += 1
    print(f'select: {n} files')


if __name__ == '__main__':
    if sys.argv[1] == 'select':
        select(sys.argv[2].rstrip('/'), sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
    else:
        {'crema': crema, 'aot': aot}[sys.argv[1]](sys.argv[2].rstrip('/'))
