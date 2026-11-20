"""GX gather pipe inline (format version 8): translated code's stores to the Wii's write gather pipe
(guest 0xCC008000-0xCC00801F, the GX FIFO) go straight into the runtime's gather buffer instead of
through aot_hw_stN -> aot_imports.hwWrite -> mmioHwWrite -> fifoWrite (three calls a word; ~1.6 million
words a second in SSPE52's village, ~13% of the PS4's game thread in v00.16). Three new imports: the
buffer (nullptr: no inline path, as before), its fill, and the burst the runtime makes of the first
32 bytes once there are that many (mmio.cpp mmioGatherBurst).

  patch-aot-gp.py crema <crema>/src   BackendC's prelude (new translations), AotFormat.h's version,
                                      the loader's imports (PPCRecompiler.cpp: g_cremaAotGp*)
  patch-aot-gp.py aot <dir>           C++ already generated (aot_*.cpp, aot_manifest.cpp): the same
                                      prelude and version, so no new translation run is needed

Needs patch-aot-hw.py (format 6) and patch-aot-sys.py (format 7) first. Idempotent."""
import glob
import sys

MARK = '(8) patch-aot-gp.py'

OLD_IMPORTS_END = '\tuint32 (*sys)(uint8* h, uint32 at);\n};\n'
NEW_IMPORTS_END = ('\tuint32 (*sys)(uint8* h, uint32 at);\n'
    '\t// (8) patch-aot-gp.py: the GX gather pipe (guest 0xCC008000-0xCC00801F) written inline - its\n'
    '\t// buffer (the bytes as the guest wrote them; nullptr: through hwWrite), its fill, and the burst\n'
    '\t// the runtime makes of the first 32 bytes once there are that many\n'
    '\tuint8* gp;\n'
    '\tuint32* gpFill;\n'
    '\tvoid (*gpBurst)();\n'
    '};\n')

OLD_STORES = ('static inline void aot_st32_(const uint8* m, uint8* p, uint32 v) { if (AOT_HW(p)) aot_hw_st32((uint32)(p - m), v); else *(volatile uint32*)p = v; }\n'
    'static inline void aot_st16_(const uint8* m, uint8* p, uint16 v) { if (AOT_HW(p)) aot_hw_st16((uint32)(p - m), v); else *(volatile uint16*)p = v; }\n'
    'static inline void aot_st8_(const uint8* m, uint8* p, uint8 v) { if (AOT_HW(p)) aot_hw_st8((uint32)(p - m), v); else *(volatile uint8*)p = v; }\n')
NEW_STORES = ('// (8) the gather pipe: appended to the runtime\'s buffer here (values in memory byte order, as the\n'
    '// guest\'s bytes), the runtime bursts it every 32 bytes; anything else in the hardware area as before\n'
    'static inline void aot_gp(const void* v, uint32 n) { uint32 f = *aot_imports.gpFill; __builtin_memcpy(aot_imports.gp + f, v, n); f += n; *aot_imports.gpFill = f; if (__builtin_expect(f >= 32, 0)) aot_imports.gpBurst(); }\n'
    '#define AOT_GP(p) ((((uint32)((p) - m)) & ~31u) == 0xCC008000u && aot_imports.gp)\n'
    'static inline void aot_st32_(const uint8* m, uint8* p, uint32 v) { if (AOT_HW(p)) { if (AOT_GP(p)) aot_gp(&v, 4); else aot_hw_st32((uint32)(p - m), v); } else *(volatile uint32*)p = v; }\n'
    'static inline void aot_st16_(const uint8* m, uint8* p, uint16 v) { if (AOT_HW(p)) { if (AOT_GP(p)) aot_gp(&v, 2); else aot_hw_st16((uint32)(p - m), v); } else *(volatile uint16*)p = v; }\n'
    'static inline void aot_st8_(const uint8* m, uint8* p, uint8 v) { if (AOT_HW(p)) { if (AOT_GP(p)) aot_gp(&v, 1); else aot_hw_st8((uint32)(p - m), v); } else *(volatile uint8*)p = v; }\n')


def sub_once(text, old, new, what, path):
    if text.count(old) != 1:
        sys.exit(f'patch-aot-gp: {"not found" if old not in text else "not unique"} ({what}) in {path}')
    return text.replace(old, new, 1)


def patch_prelude(text, path):
    text = sub_once(text, OLD_IMPORTS_END, NEW_IMPORTS_END, 'imports end', path)
    return sub_once(text, OLD_STORES, NEW_STORES, 'stores', path)


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
        return sub_once(s, 'constexpr uint32 kCremaAotVersion = 7; // (sys)',
            '// 8: the GX gather pipe written inline through the imports\' gp/gpFill/gpBurst ((8) patch-aot-gp.py)\n'
            'constexpr uint32 kCremaAotVersion = 8; // (gp)', 'version', p)
    n += rw(f'{rec}/BackendC/AotFormat.h', fmt)

    def loader(s, p):
        s = sub_once(s, '\t\tuint32 (*sys)(uint8* h, uint32 at); // g_cremaAotSys ((7) patch-aot-sys.py)\n\t};\n',
            '\t\tuint32 (*sys)(uint8* h, uint32 at); // g_cremaAotSys ((7) patch-aot-sys.py)\n'
            '\t\tuint8* gp; // g_cremaAotGp ((8) patch-aot-gp.py)\n'
            '\t\tuint32* gpFill;\n'
            '\t\tvoid (*gpBurst)();\n\t};\n', 'loader imports', p)
        s = sub_once(s, 'uint32 (*g_cremaAotSys)(uint8* h, uint32 at) = nullptr;\n',
            'uint32 (*g_cremaAotSys)(uint8* h, uint32 at) = nullptr;\n'
            '// (8) patch-aot-gp.py: the runtime\'s GX gather buffer for translated code\'s inline writes, its\n'
            '// fill and its burst (nullptr: the writes go through hwWrite, as before)\n'
            'uint8* g_cremaAotGp = nullptr;\n'
            'uint32* g_cremaAotGpFill = nullptr;\n'
            'void (*g_cremaAotGpBurst)() = nullptr;\n', 'globals', p)
        return sub_once(s, '\t\timports.sys = g_cremaAotSys ? g_cremaAotSys : CremaAotSysLeave;\n',
            '\t\timports.sys = g_cremaAotSys ? g_cremaAotSys : CremaAotSysLeave;\n'
            '\t\timports.gp = g_cremaAotGp && g_cremaAotGpFill && g_cremaAotGpBurst ? g_cremaAotGp : nullptr;\n'
            '\t\timports.gpFill = g_cremaAotGpFill;\n'
            '\t\timports.gpBurst = g_cremaAotGpBurst;\n', 'fill', p)
    n += rw(f'{rec}/PPCRecompiler.cpp', loader)
    print(f'crema: {n} files patched')


def aot(d):
    n = 0
    for path in sorted(glob.glob(f'{d}/aot_*.cpp')):
        def gen(s, p):
            s = patch_prelude(s, p)
            if p.endswith('aot_manifest.cpp'):
                s = sub_once(s, "'O', 'T', '0', '1'}, 7u,", "'O', 'T', '0', '1'}, 8u,", 'manifest version', p)
            return s
        n += rw(path, gen)
    print(f'aot: {n} files patched')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('crema', 'aot'):
        sys.exit(__doc__)
    {'crema': crema, 'aot': aot}[sys.argv[1]](sys.argv[2].rstrip('/'))
