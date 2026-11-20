"""Guest memory the compiler may optimize (BackendC, no change to the AOT format or imports).

1. In functions without the hardware check (all but the ~200 that patch-aot-hw.py select gives
   AOT_HWCHECK 1), guest loads and stores are plain memory accesses (memcpy: any alignment) instead of
   volatile ones, so the compiler can keep a loaded value, forward a store to a later load of the same
   place, and schedule them. v00.19's busiest functions spent 35% of their samples on guest memory
   accesses. The functions with the check keep volatile accesses (hardware registers, the GX FIFO).
2. The translated functions' memory base is no longer __restrict: an opaque call (another translated
   function, the runtime's imports - the interpreter, the GX FIFO's bursts) may read or write guest
   memory, so no store moves past one (the GX thread reads guest memory the game wrote before its draw
   commands). hCPU (h) stays __restrict.
3. stwcx. (a store conditional on the lwarx reservation): a compare and a store, not a locked
   compare-exchange (a full barrier, ~20 cycles on the Jaguar): only the game's thread writes guest
   memory, so nothing can come between them. 3,423 sites (refcounts, the OS's atomics).

Translated code that waits on memory (a flag an interrupt sets) still sees it change: every loop has
the cycle check that leaves to the scheduler (interrupts are taken there) and comes back in with a
fresh load.

  patch-aot-mem.py crema <crema>/src   BackendC.cpp (new translations)
Idempotent."""
import sys

MARK = 'patch-aot-mem.py'

PAIRS = [
    ('''static inline uint32 aot_ld32p(const uint8* p) { return *(const volatile uint32*)p; }
static inline uint16 aot_ld16p(const uint8* p) { return *(const volatile uint16*)p; }
static inline uint8 aot_ld8p(const uint8* p) { return *(const volatile uint8*)p; }
static inline uint64 aot_ld64p(const uint8* p) { return *(const volatile uint64*)p; }
static inline void aot_st32p(uint8* p, uint32 v) { *(volatile uint32*)p = v; }
static inline void aot_st16p(uint8* p, uint16 v) { *(volatile uint16*)p = v; }
static inline void aot_st8p(uint8* p, uint8 v) { *(volatile uint8*)p = v; }
static inline void aot_st64p(uint8* p, uint64 v) { *(volatile uint64*)p = v; }''',
     '''// (patch-aot-mem.py) plain accesses (any alignment) the compiler may keep, forward and schedule
static inline uint32 aot_ld32p(const uint8* p) { uint32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline uint16 aot_ld16p(const uint8* p) { uint16 v; __builtin_memcpy(&v, p, 2); return v; }
static inline uint8 aot_ld8p(const uint8* p) { return *p; }
static inline uint64 aot_ld64p(const uint8* p) { uint64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline void aot_st32p(uint8* p, uint32 v) { __builtin_memcpy(p, &v, 4); }
static inline void aot_st16p(uint8* p, uint16 v) { __builtin_memcpy(p, &v, 2); }
static inline void aot_st8p(uint8* p, uint8 v) { *p = v; }
static inline void aot_st64p(uint8* p, uint64 v) { __builtin_memcpy(p, &v, 8); }'''),
    ('''			out += "AOT_FN uint32 " + functionName + "(uint8* __restrict h, uint8* __restrict m, uint32 entry)\\n{\\n";''',
     '''			// (patch-aot-mem.py: m not __restrict - opaque calls may read and write guest memory)
			out += "AOT_FN uint32 " + functionName + "(uint8* __restrict h, uint8* m, uint32 entry)\\n{\\n";'''),
    ('''				line("{ uint32 expected = " + R(op.regCompareValue) + "; " + R(op.regBoolOut) + " = __atomic_compare_exchange_n((uint32*)(m + (uint64)" + R(op.regEA) +
					"), &expected, " + R(op.regWriteValue) + ", false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1u : 0u; }");''',
     '''				// (patch-aot-mem.py: a compare and a store - only the game's thread writes guest memory)
				line("{ uint8* const p_ = m + (uint64)" + R(op.regEA) + "; uint32 o_; __builtin_memcpy(&o_, p_, 4); if (o_ == " + R(op.regCompareValue) +
					") { const uint32 w_ = " + R(op.regWriteValue) + "; __builtin_memcpy(p_, &w_, 4); " + R(op.regBoolOut) + " = 1u; } else " + R(op.regBoolOut) + " = 0u; }");'''),
]


def sub_once(text, old, new, path):
    if text.count(old) != 1:
        sys.exit(f'patch-aot-mem: {"not found" if old not in text else "not unique"} in {path}: {old[:70]}')
    return text.replace(old, new, 1)


def main():
    if len(sys.argv) != 3 or sys.argv[1] != 'crema':
        sys.exit(__doc__)
    path = sys.argv[2] + '/Cafe/HW/Espresso/Recompiler/BackendC/BackendC.cpp'
    s = open(path, encoding='utf-8', newline='').read()
    if MARK in s:
        print(path, 'already patched')
        return
    for old, new in PAIRS:
        s = sub_once(s, old, new, path)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print(path, 'patched')


if __name__ == '__main__':
    main()
