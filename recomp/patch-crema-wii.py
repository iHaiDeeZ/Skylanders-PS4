"""Patches a copy of Crema's PowerPC recompiler (Cemu's) so it compiles Wii code in MEM1
(0x80000000-0x81800000) as well as the Wii U code area (0x00000000-0x10000000).

The x86 JIT's code reaches the jump table as [R15 + guestIP*2 + table] with R15 = the instance data,
so for a Wii address the slot is 4 GiB above the struct. PPCRecompiler_reserveInstanceData reserves
the struct and that part of the table as one span; the emitted code stays as it is, and slot 0 (the
exit for LEAVE and cycle checks) stays in the struct. C++ reaches slots through PPC_REC_JUMP_SLOT.

Usage: python3 patch-crema-wii.py <copy>/src     (idempotent: a patched tree is left as it is)"""
import re, sys

SRC = sys.argv[1].rstrip('/')
H = f'{SRC}/Cafe/HW/Espresso/Recompiler/PPCRecompiler.h'
CPP = f'{SRC}/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp'
MARK = 'PPC_REC_WII_CODE_START'

def sub_once(text, old, new, what):
    if old not in text:
        sys.exit(f'patch-crema-wii: not found ({what}): {old[:80]!r}')
    return text.replace(old, new, 1)

# (its own step, also on an already patched tree) PPCRecompiler_disableAt: the translated function
# at an address switched off, so its callers leave to the runtime at its entry (the runtime does
# that function natively). PPCRecompiler_invalidateRange does nothing here: the runtime never runs
# PPCRecompiler_init, so the recompiler's 'initialized' stays false.
DISABLE_MARK = 'void PPCRecompiler_disableAt('
c0 = open(CPP).read()
if DISABLE_MARK not in c0:
    c0 += ('\n// Wii runtime: the translated function at an address switched off (its jump table entries reset,\n'
        '// its direct-call flag cleared), without PPCRecompiler_init\n'
        'void PPCRecompiler_disableAt(uint32 address)\n{\n'
        '\ts_ppcRecompilerState.recompilerSpinlock.lock();\n'
        '\tuint32 rStart, rEnd;\n\tPPCRecFunction_t* rFunc;\n'
        '\twhile (s_ppcRecompilerState.functionStorage.findFirstRange(address, address + 4, rStart, rEnd, rFunc))\n'
        '\t\tPPCRecompiler_deleteFunction(rFunc);\n'
        '\ts_ppcRecompilerState.recompilerSpinlock.unlock();\n}\n')
    open(CPP, 'w').write(c0)
    print('added PPCRecompiler_disableAt')
# (its own step too) CremaAotOwnerEntry: the translated function a host address is in (its entry),
# for the runtime's diagnostics (which functions' hardware accesses still trap)
OWNER_MARK = 'bool CremaAotOwnerEntry('
c0 = open(CPP).read()
if OWNER_MARK not in c0:
    c0 += ('\n// Wii runtime: the translated function (its entry address) a host address is in\n'
        'bool CremaAotOwnerEntry(uint64 rip, uint32& entry)\n{\n'
        '\tuintptr_t start, end;\n'
        '\treturn CremaAotOwner(rip, entry, start, end);\n}\n')
    open(CPP, 'w').write(c0)
    print('added CremaAotOwnerEntry')

h = open(H).read()
if MARK in h:
    print('already patched')
    sys.exit(0)

h = sub_once(h, '#define PPC_REC_CODE_AREA_SIZE\t\t(PPC_REC_CODE_AREA_END - PPC_REC_CODE_AREA_START)\n',
    '#define PPC_REC_CODE_AREA_SIZE\t\t(PPC_REC_CODE_AREA_END - PPC_REC_CODE_AREA_START)\n'
    '// Wii (Skylanders): code in MEM1 too. Its jump table slots are 4 GiB above the instance data, where\n'
    '// the emitted [R15 + guestIP*2 + table] finds them; PPCRecompiler_reserveInstanceData reserves them.\n'
    '#define PPC_REC_WII_CODE_START\t\t(0x80000000u)\n'
    '#define PPC_REC_WII_CODE_END\t\t(0x81800000u)\n'
    'inline bool PPCRecompiler_isCodeAddress(uint64 a)\n'
    '{\n'
    '\treturn a < PPC_REC_CODE_AREA_END || (a >= PPC_REC_WII_CODE_START && a < PPC_REC_WII_CODE_END);\n'
    '}\n'
    '// a range [start, end) inside one code area\n'
    'inline bool PPCRecompiler_isCodeRange(uint64 start, uint64 end)\n'
    '{\n'
    '\treturn end > start && PPCRecompiler_isCodeAddress(start) && PPCRecompiler_isCodeAddress(end - 1) &&\n'
    '\t\t((start < PPC_REC_CODE_AREA_END) == (end - 1 < PPC_REC_CODE_AREA_END));\n'
    '}\n', 'area defines')
h = sub_once(h, 'extern PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;\n',
    'extern PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;\n'
    '// jump table slot i (= guest address / 4; past the struct\'s own table for Wii addresses)\n'
    '#define PPC_REC_JUMP_SLOT(i) (((PPCREC_JUMP_ENTRY*)(void*)ppcRecompilerInstanceData)[(size_t)(i)])\n'
    '// the instance data reserved with room for the Wii slots 4 GiB above it (nullptr: not possible)\n'
    'PPCRecompilerInstanceData_t* PPCRecompiler_reserveInstanceData();\n', 'extern instance')
open(H, 'w').write(h)

c = open(CPP).read()
# every C++ access to a slot by guest address
c, n = re.subn(r'ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable\[([^\]]+)\]', r'PPC_REC_JUMP_SLOT(\1)', c)
print(f'jump table accesses: {n}')
# range checks
c = sub_once(c, '\tif (range.startAddress >= PPC_REC_CODE_AREA_END)\n',
    '\tif (!PPCRecompiler_isCodeRange(range.startAddress, (uint64)range.startAddress + range.length))\n', 'recompileFunction')
c = sub_once(c, '\tif (startAddr >= PPC_REC_CODE_AREA_SIZE)\n\t\treturn;\n',
    '\tif (!PPCRecompiler_isCodeAddress(startAddr))\n\t\treturn;\n', 'invalidateRange')
c = sub_once(c, 'rangeEnd > PPC_REC_CODE_AREA_SIZE || head.entry >= PPC_REC_CODE_AREA_SIZE',
    '!PPCRecompiler_isCodeRange(head.rangeStart, rangeEnd) || !PPCRecompiler_isCodeAddress(head.entry)', 'cache head')
c = sub_once(c, 'ppc >= PPC_REC_CODE_AREA_SIZE', '!PPCRecompiler_isCodeAddress(ppc)', 'cache entries')
c = sub_once(c, 'rangeEnd > PPC_REC_CODE_AREA_SIZE || rec.entry >= PPC_REC_CODE_AREA_SIZE',
    '!PPCRecompiler_isCodeRange(rec.rangeStart, rangeEnd) || !PPCRecompiler_isCodeAddress(rec.entry)', 'aot record')
c = sub_once(c, 'entries[i] >= PPC_REC_CODE_AREA_SIZE', '!PPCRecompiler_isCodeAddress(entries[i])', 'aot entries')
# the 4 MiB block mask up to the end of MEM1
c = sub_once(c, '    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;\n',
    '    return (PPC_REC_WII_CODE_END + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;\n', 'block count')
# allocateRange: Wii ranges clamp to MEM1, not to the Wii U code area
c = sub_once(c,
    '\tstartAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);\n'
    '\tendAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);\n',
    '\tconst uint32 areaEnd = startAddress >= PPC_REC_WII_CODE_START ? PPC_REC_WII_CODE_END : (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE;\n'
    '\tstartAddress = std::min(startAddress, areaEnd);\n'
    '\tendAddress = std::min(endAddress, areaEnd);\n', 'allocateRange clamp')
c = sub_once(c, 'void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)\n{',
    'PPCRecompilerInstanceData_t* PPCRecompiler_reserveInstanceData()\n'
    '{\n'
    '\t// one reservation from the struct to the end of the Wii slots: nothing else can land between them\n'
    '\tstatic_assert(sizeof(PPCRecompilerInstanceData_t) <= (size_t)PPC_REC_WII_CODE_START * 2);\n'
    '\tconst size_t size = (size_t)PPC_REC_WII_CODE_END * 2;\n'
    '\treturn (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, size, MemMapper::PAGE_PERMISSION::P_RW);\n'
    '}\n\n'
    'void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)\n{', 'reserveInstanceData')
# an instruction the recompiler can't do no longer fails the whole function: the function leaves to the
# interpreter there (as tw does: its own segment, LEAVE at its address). On the Wii these are the
# zero words CodeWarrior leaves after calls that never return (never run) and supervisor instructions
# (mfmsr/mtmsr, SPRs, cache and FPSCR ops) that the runtime's interpreter does.
IMLGEN = f'{SRC}/Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGen.cpp'
g = open(IMLGEN).read()
g = sub_once(g,
    '\t\t\tCremaNoteUnsupported(addressOfCurrentInstruction, _swapEndianU32(*(uint32*)(memory_base + addressOfCurrentInstruction)));\n'
    '#endif\n'
    '\t\t\tppcImlGenContext.currentOutputSegment = nullptr;\n'
    '\t\t\treturn false;\n',
    '\t\t\tCremaNoteUnsupported(addressOfCurrentInstruction, _swapEndianU32(*(uint32*)(memory_base + addressOfCurrentInstruction)));\n'
    '#endif\n'
    '\t\t\t// (Wii: leave to the interpreter at this instruction, as tw does, and go on with the next one)\n'
    '\t\t\tppcImlGenContext.ppcAddressOfCurrentInstruction = addressOfCurrentInstruction;\n'
    '\t\t\tppcImlGenContext.currentInstruction = (uint32*)(memory_base + addressOfCurrentInstruction) + 1;\n'
    '\t\t\tPPCRecompilerImlGen_TW(&ppcImlGenContext, 0);\n'
    '\t\t\tcontinue;\n', 'leave at unsupported')
# opcode 1 is Cemu's HLE instruction (calls into the Wii U OS); on Broadway it is illegal, so in a Wii
# game it can only be data: unsupported (leave) instead of an HLE call
g = sub_once(g, '\tcase 1:\n\t\tif (PPCRecompilerImlGen_HLE(ppcImlGenContext, opcode) == false)\n\t\t\tunsupportedInstructionFound = true;\n',
    '\tcase 1:\n\t\t// (Wii: no HLE instruction; opcode 1 is illegal on Broadway)\n\t\tunsupportedInstructionFound = true;\n', 'no HLE opcode')
open(IMLGEN, 'w').write(g)

# BackendC: one case per entry address. With the leave above, an unsupported instruction that starts
# an enterable block gives two enterable segments at its address (the block's, falling into the
# leave's); the last one is kept, as the JIT's jump table ends up with the last one too (same cycles).
BACKENDC = f'{SRC}/Cafe/HW/Espresso/Recompiler/BackendC/BackendC.cpp'
b = open(BACKENDC).read()
b = sub_once(b,
    '\t\t\tline("switch (entry)");\n\t\t\tline("{");\n\t\t\tfor (size_t s = 0; s < segments.size(); s++)\n\t\t\t{\n'
    '\t\t\t\tif (!segments[s]->isEnterable)\n\t\t\t\t\tcontinue;\n',
    '\t\t\tline("switch (entry)");\n\t\t\tline("{");\n\t\t\tstd::map<uint32, size_t> lastAt;\n'
    '\t\t\tfor (size_t s = 0; s < segments.size(); s++)\n'
    '\t\t\t\tif (segments[s]->isEnterable)\n'
    '\t\t\t\t\tlastAt[segments[s]->enterPPCAddress] = s;\n'
    '\t\t\tfor (size_t s = 0; s < segments.size(); s++)\n\t\t\t{\n'
    '\t\t\t\tif (!segments[s]->isEnterable || lastAt[segments[s]->enterPPCAddress] != s)\n\t\t\t\t\tcontinue;\n',
    'one case per entry')
open(BACKENDC, 'w').write(b)

# the interpreter for Wii code: supervisor instructions (SRR0/SRR1, rfi, BATs, HID, DMA, SPRGs...) as the
# full interpreter has them, memory as the slim one (no MMU: addresses used as they are; the runtime maps
# MEM1/MEM2 at their physical addresses too, so code run in real mode reads the same memory, and the Wii's
# fixed BAT layout is what the cached/uncached views already are)
ITP = f'{SRC}/Cafe/HW/Espresso/Interpreter/PPCInterpreterImpl.cpp'
t = open(ITP).read()
t = sub_once(t, '// Full interpreter, supports most PowerPC features\n',
    '// Wii (Skylanders): supervisor mode without an MMU (see patch-crema-wii.py)\n'
    'namespace coreinit { uint64 OSGetSystemTime(); }\n'
    'class PPCItpWiiNoMMU : public PPCItpCafeOSUsermode\n'
    '{\n'
    'public:\n'
    '\tstatic const bool allowSupervisorMode = true;\n'
    '\t// (no MMU: an address is its own translation)\n'
    '\tinline static uint32 ppcMem_translateVirtualDataToPhysicalAddr(PPCInterpreter_t* hCPU, uint32 vAddr) { return vAddr; }\n'
    '\t// (the time base: the runtime\'s, as translated code\'s mftb reads it)\n'
    '\tinline static uint64 getTB(PPCInterpreter_t* hCPU) { return coreinit::OSGetSystemTime(); }\n'
    '};\n\n'
    'void PPCInterpreterWii_executeInstruction(PPCInterpreter_t* hCPU)\n'
    '{\n'
    '\tPPCInterpreterContainer<PPCItpWiiNoMMU>::executeInstruction(hCPU);\n'
    '}\n\n'
    '// Full interpreter, supports most PowerPC features\n', 'wii interpreter')
open(ITP, 'w').write(t)

# the decrementer belongs to the runtime (it counts down at the time base's rate and raises the
# decrementer exception): DEC writes and reads go through hooks; PVR reads give Broadway's
SPRH = f'{SRC}/Cafe/HW/Espresso/Interpreter/PPCInterpreterSPR.hpp'
s = open(SPRH).read()
dec_read = re.compile(r'\tcase SPR_DEC:\n\t\t// special handling for DEC register\n\t\{\n\t\tassert_dbg\(\);.*?\n\t\}\n\tbreak;\n', re.S)
s, n = dec_read.subn('\tcase SPR_DEC:\n\t\t// (Wii: the runtime\'s decrementer)\n\t\tv = ::g_wiiDecRead ? ::g_wiiDecRead() : 0;\n\t\tbreak;\n', s)
if n == 0:
    sys.exit('patch-crema-wii: DEC read not found')
s = sub_once(s, '\tcase SPR_PVR:\n\t\tassert_dbg();\n', '\tcase SPR_PVR:\n\t\tv = 0x00087102; // (Wii: Broadway)\n\t\tbreak;\n', 'PVR read')
# the supervisor getter (the one the Wii interpreter uses) lacks registers its setter has: SRR0/SRR1
# (the OS's exception vectors read them: without them every exception looked unrecoverable), the
# locked-cache DMA registers (DMAL's trigger bit reads clear: done), and the decrementer
s = sub_once(s, '\tdefault:\n\t\tdebug_printf("[C%d] Get unhandled SPR %d\\n", hCPU->spr.UPIR, spr);\n',
    '\tcase SPR_SRR0:\n\t\tv = hCPU->sprExtended.srr0;\n\t\tbreak;\n'
    '\tcase SPR_SRR1:\n\t\tv = hCPU->sprExtended.srr1;\n\t\tbreak;\n'
    '\tcase SPR_DMAU:\n\t\tv = hCPU->sprExtended.dmaU;\n\t\tbreak;\n'
    '\tcase SPR_DMAL:\n\t\tv = hCPU->sprExtended.dmaL & ~2u;\n\t\tbreak;\n'
    '\tcase SPR_DEC:\n\t\tv = ::g_wiiDecRead ? ::g_wiiDecRead() : 0;\n\t\tbreak;\n'
    '\tdefault:\n\t\tdebug_printf("[C%d] Get unhandled SPR %d\\n", hCPU->spr.UPIR, spr);\n', 'supervisor getter')
# (the supervisor setter's own setDEC is an empty stub: mtdec went nowhere and OS alarms never fired)
s = sub_once(s, 'static void setDEC(PPCInterpreter_t* hCPU, uint32 newValue)\n{\n',
    'static void setDEC(PPCInterpreter_t* hCPU, uint32 newValue)\n{\n\tif (::g_wiiDecWrite)\n\t\t::g_wiiDecWrite(newValue);\n', 'DEC setter')
# HID2 is kept (it read 0): the OS's FPU save/load (__OSSaveFPUContext) checks HID2.PSE and skips the
# paired singles' second halves without it, so ps1 was lost on every thread switch. Power-on value
# after the boot ROM: LSQE | WPE | PSE.
s = sub_once(s, 'static uint32 getHID2(PPCInterpreter_t* hCPU)\n{\n\tdebug_printf("Get HID2 IP 0x%08x\\n", hCPU->instructionPointer);\n\treturn 0; // todo\n}\n',
    'static uint32 getHID2(PPCInterpreter_t* hCPU)\n{\n\treturn ::g_wiiHID2;\n}\n', 'HID2 getter')
s = sub_once(s, 'static void setHID2(PPCInterpreter_t* hCPU, uint32 newValue)\n{\n\t// todo\n',
    'static void setHID2(PPCInterpreter_t* hCPU, uint32 newValue)\n{\n\t::g_wiiHID2 = newValue;\n', 'HID2 setter')
open(SPRH, 'w').write(s)
# (declared at file scope: the SPR header is included inside the interpreter's class template)
t = open(ITP).read()
t = sub_once(t, 'class PPCItpCafeOSUsermode\n',
    'extern uint32 (*g_wiiDecRead)(); // (Wii: the runtime\'s decrementer, PPCInterpreterMain.cpp)\n'
    'extern void (*g_wiiDecWrite)(uint32);\n'
    'extern uint32 g_wiiHID2; // (Wii: HID2, kept for the OS\'s paired-single save)\n\n'
    'class PPCItpCafeOSUsermode\n', 'DEC read declaration')
open(ITP, 'w').write(t)
MAIN = f'{SRC}/Cafe/HW/Espresso/Interpreter/PPCInterpreterMain.cpp'
m = open(MAIN).read()
m = sub_once(m, 'void PPCInterpreter_setDEC(PPCInterpreter_t* hCPU, uint32 newValue)\n{\n',
    '// Wii: the runtime\'s decrementer (nullptr: as before)\n'
    'uint32 (*g_wiiDecRead)() = nullptr;\n'
    'void (*g_wiiDecWrite)(uint32) = nullptr;\n'
    'uint32 g_wiiHID2 = 0xE0000000; // LSQE | WPE | PSE\n\n'
    'void PPCInterpreter_setDEC(PPCInterpreter_t* hCPU, uint32 newValue)\n{\n'
    '\tif (g_wiiDecWrite)\n\t\tg_wiiDecWrite(newValue);\n', 'DEC write')
open(MAIN, 'w').write(m)

# diagnostics: JT_ALL_UNSUPPORTED=1 logs every unsupported instruction (address and word), not only
# the 1st/10th/100th of each kind
c = sub_once(c, '\tif (count == 1 || count == 10 || count == 100)\n\t{\n\t\tchar line[160];\n'
    '\t\tsnprintf(line, sizeof(line), "recompiler: unsupported instruction',
    '\tstatic const bool s_all = getenv("JT_ALL_UNSUPPORTED") != nullptr;\n'
    '\tif (s_all || count == 1 || count == 10 || count == 100)\n\t{\n\t\tchar line[160];\n'
    '\t\tsnprintf(line, sizeof(line), "recompiler: unsupported instruction', 'all unsupported')
open(CPP, 'w').write(c)
# Wii: Broadway's quantization registers are GQR0-7 (SPR 912-919); Crema has only the Wii U's user-mode
# copies UGQR0-7 (896-903), which psq_l/psq_st read. Without this, every mtspr GQRn (games' set-up,
# the OS's per-thread restore) was dropped and paired-single quantized loads and stores ran as plain
# floats: floats (even infinities) landed in s16 vertex buffers, and the level drew as spikes.
GQR_REMAP = '\tif (spr >= 912 && spr <= 919)\n\t\tspr -= 16; // (Wii: GQR0-7 are UGQR0-7 here)\n'
OPC = f'{SRC}/Cafe/HW/Espresso/Interpreter/PPCInterpreterOPC.hpp'
o = open(OPC).read()
o = sub_once(o, '\tspr = spr1 | (spr2 << 5);\n\t// copy SPR\n', '\tspr = spr1 | (spr2 << 5);\n' + GQR_REMAP + '\t// copy SPR\n', 'MFSPR GQR')
o = sub_once(o, '\tspr = spr1 | (spr2 << 5);\n\tPPCSpr_set(hCPU, spr, hCPU->gpr[rD]);\n',
    '\tspr = spr1 | (spr2 << 5);\n' + GQR_REMAP + '\tPPCSpr_set(hCPU, spr, hCPU->gpr[rD]);\n', 'MTSPR GQR')
open(OPC, 'w').write(o)
GEN = f'{SRC}/Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGen.cpp'
g = open(GEN).read()
for what in ('MTSPR', 'MFSPR'):
    head = f'bool PPCRecompilerImlGen_{what}(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)\n{{\n\tuint32 rD, spr1, spr2, spr;\n\tPPC_OPC_TEMPL_XO(opcode, rD, spr1, spr2);\n\tspr = spr1 | (spr2<<5);\n'
    g = sub_once(g, head, head + GQR_REMAP, f'recompiler {what} GQR')
open(GEN, 'w').write(g)
print('patched')
