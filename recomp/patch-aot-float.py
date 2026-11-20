"""Single-precision floats the compiler can see (BackendC, no change to the AOT format or imports).

Translated code keeps every FPR as a double. A single-precision op (fadds, fmuls, ps_add...) is the
double op and then a rounding to single and back, and a single load (lfs, psq_l) a widening - both
written as aot_f2d, an inline-asm vcvtss2sd the compiler can't look through. In v00.19's hottest
function (a float vertex loop) nearly every op was mulsd/addsd + vcvtsd2ss + vcvtss2sd: three
dependent conversions-and-ops where the Gekko does one.

With plain casts instead - (double)(float)x for the rounding, (double)f for the widening - LLVM's
instcombine narrows fptrunc(fadd/fsub/fmul/fdiv(fpext a, fpext b)) to the float op (exact: a double
holds a float sum, difference, product or quotient exactly enough that rounding it once more to float
gives the float op's result), so chains of single-precision ops run as float ops and the conversions
stay only where a double is needed (double ops, stores of the register, fused multiply-adds).

The results are the same: the operations are exact narrowings, and with the PS4's (and the tests')
flush-to-zero + denormals-are-zero MXCSR a float op treats a denormal input as zero exactly as the
widening did. The one difference: a denormal single loaded and stored again without arithmetic keeps
its bits (as on the Gekko) where the opaque widening made it zero.

  patch-aot-float.py crema <crema>/src   BackendC.cpp (new translations)
Needs patch-aot-sys.py (format 7) first. Idempotent."""
import sys

MARK = 'patch-aot-float.py'

PAIRS = [
    ('''						line(d + " = aot_f2d(aot_u2f(__builtin_bswap32(aot_ld32(" + addr + "))));");''',
     '''						line(d + " = (double)aot_u2f(__builtin_bswap32(aot_ld32(" + addr + ")));"); // (patch-aot-float.py: a cast the compiler sees)'''),
    ('''				case PPCREC_IML_OP_FPR_BITCAST_INT_TO_FLOAT: line(r + " = aot_f2d(aot_u2f(" + a + "));"); return true;''',
     '''				case PPCREC_IML_OP_FPR_BITCAST_INT_TO_FLOAT: line(r + " = (double)aot_u2f(" + a + ");"); return true; // (patch-aot-float.py)'''),
    ('''				case PPCREC_IML_OP_FPR_ROUND_TO_SINGLE_PRECISION_BOTTOM: line(r + " = aot_f2d((float)" + r + ");"); return true;''',
     '''				// (patch-aot-float.py: plain casts - chains of single-precision ops become float ops)
				case PPCREC_IML_OP_FPR_ROUND_TO_SINGLE_PRECISION_BOTTOM: line(r + " = (double)(float)" + r + ";"); return true;'''),
    ('''				case PPCREC_IML_OP_FPR_EXPAND_F32_TO_F64: line(r + " = aot_f2d(aot_u2f((uint32)aot_d2u(" + r + ")));"); return true;''',
     '''				case PPCREC_IML_OP_FPR_EXPAND_F32_TO_F64: line(r + " = (double)aot_u2f((uint32)aot_d2u(" + r + "));"); return true; // (patch-aot-float.py)'''),
    # stfs: a double that is exactly a float stores that float's bits - the same bits the cut below
    # gives it, and for a widened float (now most stored values) the compiler folds the test to a move
    ('''static inline uint32 aot_single_bits(double d)
{
	const uint64 b = aot_d2u(d);''',
     '''static inline uint32 aot_single_bits(double d)
{
	// (patch-aot-float.py) exactly a float: its bits (a widened float: a move once compiled)
	{
		const float f = (float)d;
		if ((double)f == d)
			return aot_f2u(f);
	}
	const uint64 b = aot_d2u(d);'''),
]


def sub_once(text, old, new, path):
    if text.count(old) != 1:
        sys.exit(f'patch-aot-float: {"not found" if old not in text else "not unique"} in {path}: {old[:70]}')
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
