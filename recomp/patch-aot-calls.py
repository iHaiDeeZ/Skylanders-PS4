"""Cheaper calls in translated code (BackendC, no change to the AOT format or its imports):

1. The registers a call gives back as it found them - EABI's non-volatile ones: r1, r2, r13, r14-r31,
   the first halves of f14-f31 (a callee's single-precision ops change the second halves and its lfd
   restores only the first), CR2-CR4 - stay in the caller's locals across the call: after a call comes
   back they are not loaded from hCPU again (the stores before the call are as before, so hCPU always
   has them at a call: a callee that saves them, the unwinding to the scheduler, the re-entries). Calls
   into CodeWarrior's register save/restore helpers (_savegpr_N/_restgpr_N/_savefpr_N/_restfpr_N, a
   range given to PPCRecompiler_cSetKeepCalls), which change them on purpose, load everything again.
   In v00.17's code ~10 loads after a typical call, about half of them such registers.
2. A function entered at its start (its callers' way in) loads only the registers its code reads from
   there - liveness that follows the kept registers across calls and the code across the in-place
   supervisor instructions of format 7 - instead of every register the function uses (its other
   entries, the return addresses the bridge comes back to, still load everything).

  patch-aot-calls.py crema <crema>/src   BackendC.cpp (PPCRecompiler_cSetKeepCalls) and its header
Then a new translation with JT_AOT_KEEP=<lo>-<hi> (the helpers' guest address range, hex), recomp/
driver.cpp. Needs patch-aot-sys.py (format 7) first. Idempotent."""
import sys

MARK = 'patch-aot-calls.py'


def sub_once(text, old, new, what, path):
    if text.count(old) != 1:
        sys.exit(f'patch-aot-calls: {"not found" if old not in text else "not unique"} ({what}) in {path}')
    return text.replace(old, new, 1)


GLOBALS_OLD = '''	bool s_abiCalls = false;
'''
GLOBALS_NEW = '''	bool s_abiCalls = false;

	// (patch-aot-calls.py) the registers a normal call gives back as it found them stay in locals across
	// calls (not loaded again after one; calls into [s_keepHelperLo, s_keepHelperHi), the register
	// save/restore helpers, load everything), and an entry at the function's start loads only what is
	// read from there (PPCRecompiler_cSetKeepCalls)
	bool s_keepCalls = false;
	uint32 s_keepHelperLo = 0, s_keepHelperHi = 0;
	int s_keepParts = 3; // (bit 0: the kept registers across calls, bit 1: the start entry's loads)

	// EABI's non-volatile registers (r1, r2, r13 too), less the second halves of f14-f31: a callee's
	// single-precision ops change those and its lfd gives back only the first
	bool Preserved(IMLName name)
	{
		if (name == PPCREC_NAME_R0 + 1 || name == PPCREC_NAME_R0 + 2 || name == PPCREC_NAME_R0 + 13)
			return true;
		if (name >= PPCREC_NAME_R0 + 14 && name < PPCREC_NAME_R0 + 32)
			return true;
		if (name >= PPCREC_NAME_FPR_HALF + 2 * 14 && name < PPCREC_NAME_FPR_HALF + 64)
			return ((name - PPCREC_NAME_FPR_HALF) & 1) == 0;
		return name >= PPCREC_NAME_CR + 8 && name < PPCREC_NAME_CR + 20;
	}
'''

FIELDS_OLD = '''		std::vector<std::vector<bool>> liveIn;
'''
FIELDS_NEW = '''		std::vector<std::vector<bool>> liveIn;
		// (patch-aot-calls.py) liveIn with the kept registers followed across calls and the code across
		// format 7's in-place supervisor instructions: what an entry at the start loads, what a call
		// coming back loads (less the kept ones); empty without s_keepCalls
		std::vector<std::vector<bool>> liveKeep;
'''

RELOAD_OLD = '''		void reload(size_t segment)
		{
			const auto& live = liveIn[segment];
			for (IMLRegID id = 0; id < live.size(); id++)
			{
				if (!live[id])
					continue;
				Field f;
				FieldOf(nameOf[id], f);
				if (f.kind == FieldKind::Temporary)
					continue;
'''
RELOAD_NEW = '''		void reload(size_t segment, bool keepPreserved = false)
		{
			// (patch-aot-calls.py: keepPreserved - a normal call came back, the kept registers are as they were)
			const auto& live = liveKeep.empty() ? liveIn[segment] : liveKeep[segment];
			for (IMLRegID id = 0; id < live.size(); id++)
			{
				if (!live[id])
					continue;
				Field f;
				FieldOf(nameOf[id], f);
				if (f.kind == FieldKind::Temporary)
					continue;
				if (keepPreserved && nameOf.count(id) && Preserved(nameOf[id]))
					continue;
'''

TRANSFER_OLD = '''			line("{ " + lookup + "uint32 next; if (" + callee + ") next = " + callee + "(h, m, " + target + "); else { next = aot_imports.hle(h, " + target +
				"); if (!next) return " + target + "; } " + text);
			reload(back);
'''
TRANSFER_NEW = '''			line("{ " + lookup + "uint32 next; if (" + callee + ") next = " + callee + "(h, m, " + target + "); else { next = aot_imports.hle(h, " + target +
				"); if (!next) return " + target + "; } " + text);
			// (patch-aot-calls.py: the kept registers stay, except after the save/restore helpers)
			reload(back, s_keepCalls && (s_keepParts & 1) && !(fixedTarget >= s_keepHelperLo && fixedTarget < s_keepHelperHi));
'''

LIVE_OLD = '''			for (size_t s = 0; s < count; s++)
				if (segments[s]->isEnterable)
					entrySegments.emplace_back(segments[s]->enterPPCAddress, s);
'''
LIVE_NEW = '''			// (patch-aot-calls.py) liveKeep: liveIn, also across a normal call (call -> the segment it comes
			// back to) for the kept registers, and across format 7's in-place supervisor instructions (the
			// LEAVE marked 7 -> the next instruction's segment) for all, as the code goes on there with its
			// locals
			if (s_keepCalls)
			{
				auto firstEntry = [&](uint32 address) -> size_t {
					for (size_t t = 0; t < count; t++)
						if (segments[t]->isEnterable && segments[t]->enterPPCAddress == address)
							return t;
					return ~(size_t)0;
				};
				std::vector<std::vector<size_t>> callSucc(count), sysSucc(count);
				for (size_t s = 0; s < count; s++)
				{
					if (!isExit[s])
						continue;
					IMLSegment* seg = segments[s];
					const IMLInstruction& last = seg->imlList.back();
					uint32 returnTo = 0, target = 0;
					if (last.operation == PPCREC_IML_MACRO_BL)
					{
						returnTo = last.op_macro.param + 4;
						target = last.op_macro.param2;
					}
					else if (last.operation == PPCREC_IML_MACRO_B_TO_REG && last.op_macro.paramU16 == 2)
						returnTo = returnAddressBefore(seg, seg->imlList.size() - 1);
					else if (last.operation == PPCREC_IML_MACRO_LEAVE && last.op_macro.paramU16 == 7)
					{
						const size_t on = firstEntry(last.op_macro.param + 4);
						IMLSegment* after = on != ~(size_t)0 ? segments[on] : nullptr;
						if (after && (!after->imlList.empty() || after->GetBranchNotTaken()))
							sysSucc[s].push_back(on);
						continue;
					}
					if (!returnTo || !(s_keepParts & 1) || (target >= s_keepHelperLo && target < s_keepHelperHi))
						continue;
					const size_t back = firstEntry(returnTo);
					if (back != ~(size_t)0)
						callSucc[s].push_back(back);
				}
				std::vector<bool> kept(maxId + 1);
				for (IMLRegID id = 0; id <= maxId; id++)
					kept[id] = nameOf.count(id) && Preserved(nameOf[id]);
				liveKeep = liveIn;
				for (bool changed = true; changed;)
				{
					changed = false;
					for (size_t s = count; s-- > 0;)
					{
						std::vector<bool> inSet = liveKeep[s];
						for (size_t t : succ[s])
							for (size_t k = 0; k <= maxId; k++)
								if (liveKeep[t][k] && !def[s][k])
									inSet[k] = true;
						for (size_t t : sysSucc[s])
							for (size_t k = 0; k <= maxId; k++)
								if (liveKeep[t][k] && !def[s][k])
									inSet[k] = true;
						for (size_t t : callSucc[s])
							for (size_t k = 0; k <= maxId; k++)
								if (kept[k] && liveKeep[t][k] && !def[s][k])
									inSet[k] = true;
						if (inSet != liveKeep[s])
						{
							liveKeep[s] = std::move(inSet);
							changed = true;
						}
					}
				}
			}

			for (size_t s = 0; s < count; s++)
				if (segments[s]->isEnterable)
					entrySegments.emplace_back(segments[s]->enterPPCAddress, s);
'''

PROLOGUE_OLD = '''			line("uint32 cmpA = 0, cmpB = 0;");
			for (IMLRegID id = 0; id <= maxId; id++)
			{
				if (!used[id])
					continue;
				Field f;
				FieldOf(nameOf[id], f);
				const std::string var = (isFloat[id] ? "d" : "i") + std::to_string(id);
				const char* type = isFloat[id] ? "double" : "uint32";
				if (f.kind == FieldKind::Temporary)
					line(std::string(type) + " " + var + " = 0;");
				else if (isFloat[id] != (f.kind == FieldKind::F64))
					return fail("register format");
				else
					line(std::string(type) + " " + var + " = " + FieldLValue(f) + ";");
			}
			line("switch (entry)");
			line("{");
			std::map<uint32, size_t> lastAt;
			for (size_t s = 0; s < segments.size(); s++)
				if (segments[s]->isEnterable)
					lastAt[segments[s]->enterPPCAddress] = s;
'''
PROLOGUE_NEW = '''			line("uint32 cmpA = 0, cmpB = 0;");
			std::map<uint32, size_t> lastAt;
			for (size_t s = 0; s < segments.size(); s++)
				if (segments[s]->isEnterable)
					lastAt[segments[s]->enterPPCAddress] = s;
			if (s_keepCalls && (s_keepParts & 2) && lastAt.count(startAddress))
			{
				// (patch-aot-calls.py) the locals declared; entered at the start, the registers read from there
				// loaded (liveKeep), entered anywhere else, all of them
				std::vector<std::string> loads;
				for (IMLRegID id = 0; id <= maxId; id++)
				{
					if (!used[id])
						continue;
					Field f;
					FieldOf(nameOf[id], f);
					const std::string var = (isFloat[id] ? "d" : "i") + std::to_string(id);
					const char* type = isFloat[id] ? "double" : "uint32";
					if (f.kind == FieldKind::Temporary)
						line(std::string(type) + " " + var + " = 0;");
					else if (isFloat[id] != (f.kind == FieldKind::F64))
						return fail("register format");
					else
					{
						line(std::string(type) + " " + var + ";");
						loads.push_back(var + " = " + FieldLValue(f) + ";");
					}
				}
				const size_t s0 = lastAt[startAddress];
				char text[64];
				snprintf(text, sizeof(text), "if (entry == 0x%08xu)", startAddress);
				line(text);
				line("{");
				for (IMLRegID id = 0; id <= maxId; id++)
				{
					if (!used[id] || !liveKeep[s0][id])
						continue;
					Field f;
					FieldOf(nameOf[id], f);
					if (f.kind != FieldKind::Temporary)
						line("\\t" + std::string(isFloat[id] ? "d" : "i") + std::to_string(id) + " = " + FieldLValue(f) + ";");
				}
				line("\\tgoto S" + std::to_string(s0) + ";");
				line("}");
				for (const std::string& l : loads)
					line(l);
			}
			else
				for (IMLRegID id = 0; id <= maxId; id++)
				{
					if (!used[id])
						continue;
					Field f;
					FieldOf(nameOf[id], f);
					const std::string var = (isFloat[id] ? "d" : "i") + std::to_string(id);
					const char* type = isFloat[id] ? "double" : "uint32";
					if (f.kind == FieldKind::Temporary)
						line(std::string(type) + " " + var + " = 0;");
					else if (isFloat[id] != (f.kind == FieldKind::F64))
						return fail("register format");
					else
						line(std::string(type) + " " + var + " = " + FieldLValue(f) + ";");
				}
			line("switch (entry)");
			line("{");
'''

# format 7's inline mfmsr loads rD's local again when the code after reads it - with the kept registers,
# also when it reads it only after a call (r31 holding the MSR across calls, then mtmsr r31: v00.19's
# first translation kept the old r31 there and the game hung)
MFMSR_OLD = '''if (name == PPCREC_NAME_R0 + rD && id < liveIn[on].size() && liveIn[on][id])'''
MFMSR_NEW = '''if (name == PPCREC_NAME_R0 + rD && id < liveIn[on].size() && (liveKeep.empty() ? liveIn[on][id] : liveKeep[on][id]))'''

SETTER_OLD = '''void PPCRecompiler_cSetAbiCalls(bool on)
'''
SETTER_NEW = '''// (patch-aot-calls.py) kept registers across calls and start-entry loads (helpers: the guest address
// range of the register save/restore helpers, whose calls load everything again)
void PPCRecompiler_cSetKeepCalls(bool on, uint32 helperLo, uint32 helperHi)
{
	s_keepCalls = on;
	s_keepHelperLo = helperLo;
	s_keepHelperHi = helperHi;
}

// which of the two (bit 0: kept registers across calls, bit 1: the start entry's loads; default both)
void PPCRecompiler_cSetKeepParts(int parts)
{
	s_keepParts = parts;
}

void PPCRecompiler_cSetAbiCalls(bool on)
'''


def patch_backend(text, path):
    text = sub_once(text, GLOBALS_OLD, GLOBALS_NEW, 'globals', path)
    text = sub_once(text, FIELDS_OLD, FIELDS_NEW, 'fields', path)
    text = sub_once(text, RELOAD_OLD, RELOAD_NEW, 'reload', path)
    text = sub_once(text, TRANSFER_OLD, TRANSFER_NEW, 'transfer', path)
    text = sub_once(text, LIVE_OLD, LIVE_NEW, 'liveness', path)
    text = sub_once(text, PROLOGUE_OLD, PROLOGUE_NEW, 'prologue', path)
    text = sub_once(text, MFMSR_OLD, MFMSR_NEW, 'mfmsr', path)
    text = sub_once(text, SETTER_OLD, SETTER_NEW, 'setter', path)
    return text


def patch_header(text, path):
    old = 'void PPCRecompiler_cSetAbiCalls(bool on);'
    new = ('void PPCRecompiler_cSetAbiCalls(bool on);\n'
           '// (patch-aot-calls.py) registers kept in locals across calls, start-entry loads\n'
           'void PPCRecompiler_cSetKeepCalls(bool on, uint32 helperLo, uint32 helperHi);\n'
           'void PPCRecompiler_cSetKeepParts(int parts);')
    return sub_once(text, old, new, 'header', path)


def rw(path, fn):
    s = open(path, encoding='utf-8', newline='').read()
    if MARK in s:
        return False
    s = fn(s, path)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    return True


def main():
    if len(sys.argv) != 3 or sys.argv[1] != 'crema':
        sys.exit(__doc__)
    src = sys.argv[2]
    backend = src + '/Cafe/HW/Espresso/Recompiler/BackendC/BackendC.cpp'
    print(backend, 'patched' if rw(backend, patch_backend) else 'already patched')
    import glob
    headers = [h for h in glob.glob(src + '/Cafe/HW/Espresso/Recompiler/**/*.h', recursive=True)
               if 'void PPCRecompiler_cSetAbiCalls(bool on);' in open(h, encoding='utf-8', errors='replace').read()]
    if not headers:
        sys.exit('patch-aot-calls: no header declares PPCRecompiler_cSetAbiCalls')
    for h in headers:
        print(h, 'patched' if rw(h, patch_header) else 'already patched')


if __name__ == '__main__':
    main()
