"""CodeWarrior's register save/restore helpers expanded where they're called (Crema's PPC -> IML step,
PPCRecompilerImlGen.cpp; no change to the AOT format, the runtime or BackendC).

The game's functions save and restore their non-volatile registers through CodeWarrior's helpers:
_savegpr_N / _restgpr_N / _savefpr_N / _restfpr_N, 0x8034F280-0x8034F3AF - one long run of
stw/lwz/stfd/lfd off r11 entered N instructions in, then blr. Translated, every one is a call: the
caller stores its changed registers to hCPU for it, the helper loads them from there (and runs its
entry checks), and after it the caller loads everything again (patch-aot-calls.py keeps registers
across calls, but not across these: they write registers). ~700 call sites of _savegpr_27/_restgpr_27
alone; in v00.21's PS4 samples the helpers' own code was the 16th and 18th busiest guest addresses.

Here a bl whose target is in the range given (PPCRecompiler_setExpandHelpers; driver.cpp JT_AOT_EXPAND=
lo-hi; 0-0, the default: Crema as before - the PS4 runtime's own JIT never sets it) and whose code up
to its blr is only lwz/stw/lfd/stfd off r11 is not a call: those instructions are decoded in the
caller's place (the same generators that translated the helper itself), and LR gets the return
address as the bl gave it. Such a bl no longer ends a basic block, and the instruction after it is no
longer an entry.

  patch-aot-expand.py crema <crema src>
Idempotent."""
import os
import sys

MARK = '(patch-aot-expand.py)'

HELPERS = r'''
// %s CodeWarrior's register save/restore helpers (_savegpr_N/_restgpr_N/_savefpr_N/_restfpr_N: a
// run of lwz/stw/lfd/stfd off r11, then blr) expanded where a bl calls them: no call, nothing stored
// for it or loaded again after it. PPCRecompiler_setExpandHelpers(lo, hi): the helpers' range (0, 0:
// none - Crema's own behaviour)
static uint32 s_expandLo = 0, s_expandHi = 0;

void PPCRecompiler_setExpandHelpers(uint32 lo, uint32 hi)
{
	s_expandLo = lo;
	s_expandHi = hi;
}

// the instructions the helper at target has before its blr (0: not one to expand)
static uint32 PPCRecompiler_expandableHelper(uint32 target)
{
	if (target < s_expandLo || target >= s_expandHi || (target & 3) != 0)
		return 0;
	for (uint32 i = 0; i < 32 && target + i * 4 < s_expandHi; i++)
	{
		const uint32 op = memory_readU32(target + i * 4);
		if (op == 0x4E800020) // blr
			return i;
		const uint32 primary = op >> 26, rA = (op >> 16) & 31;
		if ((primary != 32 && primary != 36 && primary != 50 && primary != 54) || rA != 11) // lwz stw lfd stfd, off r11
			return 0;
	}
	return 0;
}

// a bl to such a helper: its instructions here (the caller's segment), LR as the bl leaves it
static bool PPCRecompiler_expandHelperCall(ppcImlGenContext_t* ppcImlGenContext, uint32 target)
{
	const uint32 count = PPCRecompiler_expandableHelper(target);
	if (count == 0)
		return false;
	uint32* const resume = ppcImlGenContext->currentInstruction;
	const uint32 callAt = ppcImlGenContext->ppcAddressOfCurrentInstruction;
	ppcImlGenContext->currentInstruction = (uint32*)(memory_base + target);
	for (uint32 i = 0; i < count; i++)
	{
		ppcImlGenContext->ppcAddressOfCurrentInstruction = target + i * 4;
		ppcImlGenContext->cyclesSinceLastBranch++;
		if (PPCRecompiler_decodePPCInstruction(ppcImlGenContext))
			cemu_assert_suspicious();
	}
	ppcImlGenContext->currentInstruction = resume;
	ppcImlGenContext->ppcAddressOfCurrentInstruction = callAt;
	IMLReg registerLR = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_SPR0 + SPR_LR);
	ppcImlGenContext->emitInst().make_r_s32(PPCREC_IML_OP_ASSIGN, registerLR, callAt + 4);
	return true;
}

''' % MARK

B_OLD = '''	if( opcode&PPC_OPC_LK )
	{
		// function call
		ppcImlGenContext->emitInst().make_macro(PPCREC_IML_MACRO_BL, ppcImlGenContext->ppcAddressOfCurrentInstruction, jumpAddressDest, ppcImlGenContext->cyclesSinceLastBranch, IMLREG_INVALID);
		return true;
	}
	// is jump destination within recompiled function?'''
B_NEW = '''	if( opcode&PPC_OPC_LK )
	{
		// function call
		if (PPCRecompiler_expandHelperCall(ppcImlGenContext, jumpAddressDest)) // %s a save/restore helper: in place
			return true;
		ppcImlGenContext->emitInst().make_macro(PPCREC_IML_MACRO_BL, ppcImlGenContext->ppcAddressOfCurrentInstruction, jumpAddressDest, ppcImlGenContext->cyclesSinceLastBranch, IMLREG_INVALID);
		return true;
	}
	// is jump destination within recompiled function?''' % MARK

END_OLD = '''	case Espresso::PrimaryOpcode::B:
	{
		uint32 LI;
		bool AA, LK;
		Espresso::decodeOp_B(opcode, LI, AA, LK);
		if (!LK)'''
END_NEW = '''	case Espresso::PrimaryOpcode::B:
	{
		uint32 LI;
		bool AA, LK;
		Espresso::decodeOp_B(opcode, LI, AA, LK);
		if (LK && PPCRecompiler_expandableHelper(AA ? LI : LI + instructionAddress)) // %s expanded in place: not a call
			return false;
		if (!LK)''' % MARK


def main():
    if len(sys.argv) != 3 or sys.argv[1] != 'crema':
        sys.exit(__doc__)
    path = os.path.join(sys.argv[2], 'Cafe', 'HW', 'Espresso', 'Recompiler', 'PPCRecompilerImlGen.cpp')
    text = open(path, encoding='utf-8').read()
    if MARK in text:
        print('patch-aot-expand: already applied')
        return
    anchor = 'bool PPCRecompilerImlGen_B(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)\n'
    for what, old in (('the bl generator', B_OLD), ('the segment end check', END_OLD), ('PPCRecompilerImlGen_B', anchor)):
        if text.count(old) != 1:
            sys.exit(f'patch-aot-expand: {what} not found once in {path}')
    text = text.replace(B_OLD, B_NEW).replace(END_OLD, END_NEW).replace(anchor, HELPERS + anchor)
    open(path, 'w', encoding='utf-8', newline='\n').write(text)
    print('patch-aot-expand: applied to', path)


if __name__ == '__main__':
    main()
