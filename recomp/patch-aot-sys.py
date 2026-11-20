"""Supervisor instructions in place (format version 7): an instruction the recompiler can't do but
which never branches (mfmsr/mtmsr, mfspr/mtspr of the supervisor registers, segment registers, cache,
TLB, FPSCR and sync ops) no longer makes translated code leave to the interpreter there. The code
calls the runtime through a new import (sys: the interpreter runs that one instruction on hCPU) and
goes on at the next instruction, where the IML now has a segment it can be entered at.

Why: a leave goes back up the whole host stack (every translated caller returns to the bridge), the
interpreter runs the instruction, and execution comes back in through the jump table at each return
address in turn. SSPE52 calls OSDisableInterrupts/OSRestoreInterrupts ~100,000 times a second
(mfmsr + mtmsr each), and LCEnable's dcbz_l loop leaves 512 times a call (~155 calls a second): the
leaves and the bridge were most of the game thread's runtime overhead on the PS4.

  patch-aot-sys.py crema <crema>/src   PPCRecompilerImlGen.cpp (the in-place LEAVE, marked 7, and
                                       the enterable segment after it), BackendC.cpp (the call and
                                       the import), AotFormat.h's version, the loader's import
                                       (PPCRecompiler.cpp: g_cremaAotSys, null: leave as before)

The generated C++ has to be made again (new segments and entries): no 'aot' mode here.
Idempotent (a patched file is left as it is)."""
import sys

MARK = '(7) patch-aot-sys.py'


def sub_once(text, old, new, what, path):
    if text.count(old) != 1:
        sys.exit(f'patch-aot-sys: {"not found" if old not in text else "not unique"} ({what}) in {path}')
    return text.replace(old, new, 1)


def rw(path, fn):
    s = open(path, encoding='utf-8', newline='').read()
    if MARK in s:
        return False
    s = fn(s, path)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    return True


IMLGEN_HELPERS = '''// (7) patch-aot-sys.py: the Wii supervisor instructions the recompiler can't do but which never
// branch (MSR, SPRs and segment registers, cache, TLB, FPSCR and sync ops). Translated code has the
// runtime run each in place (AotImports::sys: the interpreter on hCPU) and goes on after it, instead of
// leaving to the interpreter there (a leave goes back up the whole host stack: SSPE52's
// OSDisableInterrupts/OSRestoreInterrupts are ~100,000 calls a second, LCEnable's dcbz_l loop 512 leaves
// a call). rfi, sc, traps and words that aren't instructions still leave.
static bool CremaWiiRunsInPlace(uint32 w)
{
	const uint32 op = w >> 26, xo = (w >> 1) & 0x3FF;
	if (op == 31)
	{
		switch (xo)
		{
		case 83: // mfmsr
		case 146: // mtmsr
		case 339: // mfspr
		case 467: // mtspr
		case 595: // mfsr
		case 659: // mfsrin
		case 210: // mtsr
		case 242: // mtsrin
		case 54: // dcbst
		case 86: // dcbf
		case 246: // dcbtst
		case 278: // dcbt
		case 470: // dcbi
		case 982: // icbi
		case 306: // tlbie
		case 566: // tlbsync
		case 598: // sync
		case 854: // eieio
			return true;
		}
		return false;
	}
	if (op == 4)
		return xo == 1014; // dcbz_l
	if (op == 63)
	{
		switch (xo)
		{
		case 38: // mtfsb1
		case 64: // mcrfs
		case 70: // mtfsb0
		case 134: // mtfsfi
		case 583: // mffs
		case 711: // mtfsf
			return true;
		}
		return false;
	}
	return op == 19 && xo == 150; // isync
}

// the instruction's own segment, enterable at its address as tw's leave is, with a LEAVE marked 7
// (op_macro.paramU16) that BackendC makes the in-place call; the code after it a segment enterable at the
// next address, where translated code goes on (the JIT's backends: a LEAVE as tw's)
static void CremaWiiGenInPlace(ppcImlGenContext_t* ctx)
{
	PPCIMLGen_CreateSplitSegmentAtEnd(*ctx, *ctx->currentBasicBlock);
	ctx->currentOutputSegment->SetEnterable(ctx->ppcAddressOfCurrentInstruction);
	PPCRecompilerImlGen_generateNewEmptyInstruction(ctx)->make_macro(PPCREC_IML_MACRO_LEAVE, ctx->ppcAddressOfCurrentInstruction, 0, 7, IMLREG_INVALID);
	IMLSegment* middleSeg = PPCIMLGen_CreateSplitSegmentAtEnd(*ctx, *ctx->currentBasicBlock);
	middleSeg->SetLinkBranchTaken(nullptr);
	middleSeg->SetLinkBranchNotTaken(nullptr);
	ctx->currentOutputSegment->SetEnterable(ctx->ppcAddressOfCurrentInstruction + 4);
}

'''


def imlgen(s, p):
    s = sub_once(s, 'bool PPCIMLGen_FillBasicBlock(ppcImlGenContext_t& ppcImlGenContext, PPCBasicBlockInfo& basicBlockInfo)\n',
        IMLGEN_HELPERS + 'bool PPCIMLGen_FillBasicBlock(ppcImlGenContext_t& ppcImlGenContext, PPCBasicBlockInfo& basicBlockInfo)\n', 'helpers', p)
    s = sub_once(s, '\t\t\tPPCRecompilerImlGen_TW(&ppcImlGenContext, 0);\n\t\t\tcontinue;\n',
        '\t\t\tif (CremaWiiRunsInPlace(_swapEndianU32(*(uint32*)(memory_base + addressOfCurrentInstruction))))\n'
        '\t\t\t\tCremaWiiGenInPlace(&ppcImlGenContext); // (run in place by the runtime, then on)\n'
        '\t\t\telse\n'
        '\t\t\t\tPPCRecompilerImlGen_TW(&ppcImlGenContext, 0);\n'
        '\t\t\tcontinue;\n', 'unsupported path', p)
    return s


BACKENDC_LEAVE_OLD = ('\t\t\t\tcase PPCREC_IML_MACRO_LEAVE:\n'
    '\t\t\t\t\tstoreDirty(segIndex);\n'
    '\t\t\t\t\tsnprintf(target, sizeof(target), "0x%08xu", mc.param | 1);\n')
BACKENDC_LEAVE_NEW = BACKENDC_LEAVE_OLD + (
    '\t\t\t\t\t// (7) patch-aot-sys.py: a Wii supervisor instruction (a LEAVE marked 7) done here, then on at\n'
    '\t\t\t\t\t// the next instruction\'s segment (everything changed is in hCPU now: the stores above).\n'
    '\t\t\t\t\t// Inline: the cache and sync hints (no-ops for the interpreter too), mfmsr, and mtmsr when\n'
    '\t\t\t\t\t// only EE (not turned on), RI or ME change. The rest the runtime runs in place\n'
    '\t\t\t\t\t// (aot_imports.sys), the registers the code on reads loaded again as after a call; anything\n'
    '\t\t\t\t\t// else it gives back (an interrupt to take, an MSR change for the loop, an exception): leave there\n'
    '\t\t\t\t\tif (mc.paramU16 == 7 && !s_abiCalls)\n'
    '\t\t\t\t\t{\n'
    '\t\t\t\t\t\tconst size_t on = entrySegment(mc.param + 4);\n'
    '\t\t\t\t\t\tIMLSegment* after = on != ~(size_t)0 ? ctx.segmentList2[on] : nullptr;\n'
    '\t\t\t\t\t\t// (code follows there: not the end of the function\'s range)\n'
    '\t\t\t\t\t\tif (after && (!after->imlList.empty() || after->GetBranchNotTaken()))\n'
    '\t\t\t\t\t\t{\n'
    '\t\t\t\t\t\t\tconst uint32 w = _swapEndianU32(*(uint32*)(memory_base + mc.param));\n'
    '\t\t\t\t\t\t\tconst uint32 op = w >> 26, xo = (w >> 1) & 0x3FF, rD = (w >> 21) & 31;\n'
    '\t\t\t\t\t\t\tconst size_t gpr = offsetof(PPCInterpreter_t, gpr) + 4 * rD, msr = offsetof(PPCInterpreter_t, sprExtended.msr);\n'
    '\t\t\t\t\t\t\tconst bool hint = (op == 31 && (xo == 278 || xo == 246 || xo == 54 || xo == 86 || xo == 470 || xo == 982 || xo == 598 || xo == 854)) ||\n'
    '\t\t\t\t\t\t\t\t(op == 4 && xo == 1014) || (op == 19 && xo == 150);\n'
    '\t\t\t\t\t\t\tchar text[320];\n'
    '\t\t\t\t\t\t\tif (hint)\n'
    '\t\t\t\t\t\t\t\tline("// (a cache/sync hint: nothing to do)");\n'
    '\t\t\t\t\t\t\telse if (op == 31 && xo == 83)\n'
    '\t\t\t\t\t\t\t{\n'
    '\t\t\t\t\t\t\t\t// mfmsr rD (only rD\'s local loaded again: the others still hold what hCPU has)\n'
    '\t\t\t\t\t\t\t\tsnprintf(text, sizeof(text), "*(uint32*)(h + %zu) = *(uint32*)(h + %zu); // mfmsr", gpr, msr);\n'
    '\t\t\t\t\t\t\t\tline(text);\n'
    '\t\t\t\t\t\t\t\tfor (const auto& [id, name] : nameOf)\n'
    '\t\t\t\t\t\t\t\t\tif (name == PPCREC_NAME_R0 + rD && id < liveIn[on].size() && liveIn[on][id])\n'
    '\t\t\t\t\t\t\t\t\t\tline("i" + std::to_string(id) + " = *(uint32*)(h + " + std::to_string(gpr) + ");");\n'
    '\t\t\t\t\t\t\t}\n'
    '\t\t\t\t\t\t\telse if (op == 31 && xo == 146)\n'
    '\t\t\t\t\t\t\t{\n'
    '\t\t\t\t\t\t\t\t// mtmsr rS (rD\'s field): the runtime only when it has to look (it changes no register)\n'
    '\t\t\t\t\t\t\t\tsnprintf(text, sizeof(text), "{ const uint32 o = *(uint32*)(h + %zu), v = *(uint32*)(h + %zu); // mtmsr\\n"\n'
    '\t\t\t\t\t\t\t\t\t"\\tif (((v ^ o) & ~0x9002u) == 0 && !(v & ~o & 0x8000u)) *(uint32*)(h + %zu) = v;\\n"\n'
    '\t\t\t\t\t\t\t\t\t"\\telse { const uint32 next = aot_imports.sys(h, 0x%08xu); if (next != 0x%08xu) return next; } }",\n'
    '\t\t\t\t\t\t\t\t\tmsr, gpr, msr, mc.param, mc.param + 4);\n'
    '\t\t\t\t\t\t\t\tline(text);\n'
    '\t\t\t\t\t\t\t}\n'
    '\t\t\t\t\t\t\telse\n'
    '\t\t\t\t\t\t\t{\n'
    '\t\t\t\t\t\t\t\tsnprintf(text, sizeof(text), "{ const uint32 next = aot_imports.sys(h, 0x%08xu); if (next != 0x%08xu) return next; }", mc.param, mc.param + 4);\n'
    '\t\t\t\t\t\t\t\tline(text);\n'
    '\t\t\t\t\t\t\t\treload(on);\n'
    '\t\t\t\t\t\t\t}\n'
    '\t\t\t\t\t\t\tline("goto S" + std::to_string(on) + ";");\n'
    '\t\t\t\t\t\t\treturn true;\n'
    '\t\t\t\t\t\t}\n'
    '\t\t\t\t\t}\n')

BACKENDC_DECL_OLD = 'ATTR_MS_ABI uint32 PPCRecompiler_GetTBU();\n'
BACKENDC_DECL_NEW = ('ATTR_MS_ABI uint32 PPCRecompiler_GetTBU();\n'
    'extern uint8* memory_base; // ((7) patch-aot-sys.py: the instruction words of in-place LEAVEs)\n')

PRELUDE_OLD = '\tvoid (*hwWrite)(uint32 address, uint32 value, uint32 size);\n};\n'
PRELUDE_NEW = ('\tvoid (*hwWrite)(uint32 address, uint32 value, uint32 size);\n'
    '\t// (7) patch-aot-sys.py: a supervisor instruction run in place (the interpreter, on hCPU): where\n'
    '\t// execution goes on (the next instruction\'s address: on here; anything else, bit 0: leave there)\n'
    '\tuint32 (*sys)(uint8* h, uint32 at);\n'
    '};\n')


def backendc(s, p):
    s = sub_once(s, BACKENDC_DECL_OLD, BACKENDC_DECL_NEW, 'memory_base', p)
    s = sub_once(s, BACKENDC_LEAVE_OLD, BACKENDC_LEAVE_NEW, 'LEAVE', p)
    return sub_once(s, PRELUDE_OLD, PRELUDE_NEW, 'prelude imports', p)


def fmt(s, p):
    return sub_once(s, 'constexpr uint32 kCremaAotVersion = 6; // (hwRead)',
        '// 7: supervisor instructions run in place through the imports\' sys ((7) patch-aot-sys.py)\n'
        'constexpr uint32 kCremaAotVersion = 7; // (sys)', 'version', p)


def loader(s, p):
    s = sub_once(s, '\t\tvoid (*hwWrite)(uint32 address, uint32 value, uint32 size);\n\t};\n',
        '\t\tvoid (*hwWrite)(uint32 address, uint32 value, uint32 size);\n'
        '\t\tuint32 (*sys)(uint8* h, uint32 at); // g_cremaAotSys ((7) patch-aot-sys.py)\n\t};\n', 'loader imports', p)
    s = sub_once(s, 'void (*g_cremaAotHwWrite)(uint32 address, uint32 value, uint32 size) = nullptr;\n',
        'void (*g_cremaAotHwWrite)(uint32 address, uint32 value, uint32 size) = nullptr;\n'
        '// (7) patch-aot-sys.py: the runtime running a supervisor instruction for translated code in place\n'
        '// (where execution goes on: the next address, or bit 0 set: leave there); null: leave as before\n'
        'uint32 (*g_cremaAotSys)(uint8* h, uint32 at) = nullptr;\n', 'global', p)
    s = sub_once(s, '\t// stubs: where the entries\' stubs are (nullptr: none, the functions never call each other)\n'
        '\tvoid CremaAotFillImports(',
        '\t// a supervisor instruction from translated code without a runtime that runs it: leave there\n'
        '\tuint32 CremaAotSysLeave(uint8* h, uint32 at)\n\t{\n\t\treturn at | 1;\n\t}\n\n'
        '\t// stubs: where the entries\' stubs are (nullptr: none, the functions never call each other)\n'
        '\tvoid CremaAotFillImports(', 'fallback', p)
    return sub_once(s, '\t\timports.hwWrite = g_cremaAotHwWrite ? g_cremaAotHwWrite : CremaAotHwWriteMemory;\n',
        '\t\timports.hwWrite = g_cremaAotHwWrite ? g_cremaAotHwWrite : CremaAotHwWriteMemory;\n'
        '\t\timports.sys = g_cremaAotSys ? g_cremaAotSys : CremaAotSysLeave;\n', 'fill', p)


def crema(src):
    rec = f'{src}/Cafe/HW/Espresso/Recompiler'
    n = 0
    n += rw(f'{rec}/PPCRecompilerImlGen.cpp', imlgen)
    n += rw(f'{rec}/BackendC/BackendC.cpp', backendc)
    n += rw(f'{rec}/BackendC/AotFormat.h', fmt)
    n += rw(f'{rec}/PPCRecompiler.cpp', loader)
    print(f'crema: {n} files patched')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != 'crema':
        sys.exit(__doc__)
    crema(sys.argv[2].rstrip('/'))
