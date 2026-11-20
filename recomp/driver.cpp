// jittest: random PowerPC float / paired-single programs, run by Cemu's interpreter and by Crema's
// recompiler (x86, as on the PS4: AVX, MOVBE, no BMI2) from the same state; prints one line per
// program with a hash of each result and the differences.
//   jittest seed count [length] [verbose]
#include <sys/mman.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <random>
#include <algorithm>
#include <cstring>
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Recompiler/PPCFunctionBoundaryTracker.h"
#include "Common/cpu_features.h"
#include "util/MemMapper/MemMapper.h"
#include "Cafe/HW/Espresso/Recompiler/BackendC/BackendC.h"
#include <dlfcn.h>
#include <map>

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size);
void PPCRecompiler_initPlatform();
void PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker);
void PPCInterpreterSlim_executeInstruction(PPCInterpreter_t* hCPU);
extern void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
// PPCRecompiler.cpp (JITTEST): the compiled code cache
void CremaCodeTestRecord(uint32 entry, const PPCFunctionBoundaryTracker::PPCRange_t& range, const std::vector<PPCFunctionBoundaryTracker::PPCRange_t>& hashRanges,
	const PPCRecFunction_t* func, const std::vector<std::pair<MPTR, uint32>>& entryPoints);
void CremaCodeTestWrite(const std::string& path, bool force);
void CremaCodeTestReset();
void CremaCodeTestLoad(const std::string& path, uint32 counts[5]);
void CremaCodeTestForget(uint32 start, uint32 size);
uint64 CremaCodeTestGuestHash(const std::vector<PPCFunctionBoundaryTracker::PPCRange_t>& ranges);
// ... and the translated code files (BackendC/AotFormat.h)
struct CremaAotManifest;
bool CremaAotTestLoad(const std::string& path, uint32 counts[4], std::string& why);
const CremaAotManifest* CremaAotTestOpen(const std::string& path, std::string& why);
void CremaAotTestDirectCalls(bool on);
const CremaAotManifest* CremaAotTestManifest();
extern bool g_crema_cycles_in_register; // BackendX64.cpp: Cycle Counter (JT_CYCLES_MEMORY=1: Memory)
extern bool g_crema_eabi_returns; // PPCRecompiler.cpp: Compare Results Fast (JT_EABI_RETURNS=0: Fast, Keep at Returns)
extern bool g_crema_single_loads_known; // IML/IMLOptimizer.cpp: stfs of lfs values (JT_SINGLE_LOADS=0: off, as Float Inputs: Exact)
extern bool g_crema_host_returns __attribute__((weak)); // BackendX64.cpp: Guest Returns (JT_HOST_RETURNS=0: Jump Table)

static void writeFile(const std::string& path, const std::string& text);
static uint64 s_jtPaddedJumps, s_jtWideCompares; // JT_PATTERNS
extern uint64 g_jtImlBytes[64][256], g_jtImlCount[64][256]; // BackendX64.cpp: JT_BYTES
extern uint64 g_jtSingleStoreSource[16]; // IML/IMLOptimizer.cpp: JT_BYTES
extern uint64 g_jtSingleStoreFprR[64];
extern uint32 g_jtRaMaxRounds, g_jtRaMaxRoundsAt; // IML/IMLRegisterAllocator.cpp
static bool runThroughTable(PPCInterpreter_t* cpu, uint32 start);

constexpr uint32 kCode = 0x02000000;
constexpr uint32 kReturn = 0x02F00000;
constexpr uint32 kData = 0x10000000;
constexpr uint32 kDataSize = 0x10000;
static uint32 s_currentFunction = 0; // JT_RPX: the function being compiled (crash report)

static std::mt19937_64 rng;
static uint32 rnd(uint32 n) { return (uint32)(rng() % n); }

static uint32 A(uint32 op, uint32 d, uint32 a, uint32 b, uint32 c, uint32 xo) { return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1); }
static uint32 X(uint32 op, uint32 d, uint32 a, uint32 b, uint32 xo) { return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1); }
static uint32 D(uint32 op, uint32 d, uint32 a, uint32 imm) { return (op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF); }
static uint32 PSQ(uint32 op, uint32 d, uint32 a, uint32 w, uint32 i, uint32 imm) { return (op << 26) | (d << 21) | (a << 16) | (w << 15) | (i << 12) | (imm & 0xFFF); }

static double randomSingle()
{
	switch (rnd(20))
	{
	case 0: return 0.0;
	case 1: return -0.0;
	case 2: return 1.0;
	case 3: return INFINITY;
	case 4: return -INFINITY;
	case 5: return NAN;
	case 6: return 1e-40; // a single denormal
	case 7: return 3.0e38;
	default:
	{
		uint32 bits = (rnd(2) << 31) | ((uint32)(117 + rnd(20)) << 23) | rnd(1 << 23);
		float f;
		memcpy(&f, &bits, 4);
		return f;
	}
	}
}

static bool s_singlesOnly();
static double randomValue()
{
	if (!s_singlesOnly() && rnd(8) == 0)
	{
		// not a single: a double with more mantissa bits
		uint64 bits = ((uint64)rnd(2) << 63) | ((uint64)(1013 + rnd(20)) << 52) | (rng() & ((1ull << 52) - 1));
		double d;
		memcpy(&d, &bits, 8);
		return d;
	}
	return randomSingle();
}

// JT_ENTRIES: places a compile may be entered at besides the start (word indices), by kind
enum EntryKind { kEntryLoopHead, kEntryInLoop, kEntryBranchTarget, kEntryAny, kEntryKinds };
static const char* const kEntryKindNames[kEntryKinds] = {"loop-head", "in-loop", "branch-target", "any"};

struct Program
{
	std::vector<uint32> code;
	std::vector<std::pair<uint32, EntryKind>> entryCandidates;
};

static uint32 s_regs = 8; // JT_REGS: float registers used (8: lots of aliasing, 32: register pressure)
static uint32 fr() { return rnd(s_regs); }

static std::vector<uint32> s_ops; // JT_OPS: only these instruction kinds (emitOne cases)
static uint32 s_lastKind = 0;
static bool s_singles = false;      // JT_SINGLES: registers start with single-precision values only
static bool s_noBranch = false;     // JT_NOBRANCH: straight-line programs
static bool s_loops = false;        // JT_LOOPS: also loops (CTR with bdnz, or r16 with addic. / bne)
static int s_entries = 0;           // JT_ENTRIES: extra entry points per compile, each run from too
static bool s_cut = false;          // JT_CUT: each function cut short, the rest run by the interpreter

static void emitOne(std::vector<uint32>& c)
{
	const uint32 d = fr(), a = fr(), b = fr(), cc = fr();
	s_lastKind = s_ops.empty() ? rnd(46) : s_ops[rnd((uint32)s_ops.size())];
	switch (s_lastKind)
	{
	// single precision (59)
	case 0: c.push_back(A(59, d, a, b, 0, 21)); break; // fadds
	case 1: c.push_back(A(59, d, a, b, 0, 20)); break; // fsubs
	case 2: c.push_back(A(59, d, a, 0, cc, 25)); break; // fmuls
	case 3: c.push_back(A(59, d, a, b, 0, 18)); break; // fdivs
	case 4: c.push_back(A(59, d, a, b, cc, 29)); break; // fmadds
	case 5: c.push_back(A(59, d, a, b, cc, 28)); break; // fmsubs
	case 6: c.push_back(A(59, d, a, b, cc, 31)); break; // fnmadds
	case 7: c.push_back(A(59, d, a, b, cc, 30)); break; // fnmsubs
	// double precision (63)
	case 8: c.push_back(A(63, d, a, b, 0, 21)); break; // fadd
	case 9: c.push_back(A(63, d, a, b, 0, 20)); break; // fsub
	case 10: c.push_back(A(63, d, a, 0, cc, 25)); break; // fmul
	case 11: c.push_back(A(63, d, a, b, 0, 18)); break; // fdiv
	case 12: c.push_back(A(63, d, a, b, cc, 29)); break; // fmadd
	case 13: c.push_back(A(63, d, a, b, cc, 28)); break; // fmsub
	case 14: c.push_back(A(63, d, a, b, cc, 31)); break; // fnmadd
	case 15: c.push_back(A(63, d, a, b, cc, 30)); break; // fnmsub
	case 16: c.push_back(A(63, d, a, b, cc, 23)); break; // fsel
	case 17: c.push_back(X(63, d, 0, b, 12)); break; // frsp
	case 18: c.push_back(X(63, d, 0, b, rnd(4) == 0 ? 40 : rnd(3) == 0 ? 264 : 72)); break; // fneg / fabs / fmr
	case 19: c.push_back(X(63, rnd(8) << 2, a, b, 0)); break; // fcmpu
	// paired singles (4)
	case 20: c.push_back(A(4, d, a, b, 0, 21)); break; // ps_add
	case 21: c.push_back(A(4, d, a, b, 0, 20)); break; // ps_sub
	case 22: c.push_back(A(4, d, a, 0, cc, 25)); break; // ps_mul
	case 23: c.push_back(A(4, d, a, b, 0, 18)); break; // ps_div
	case 24: c.push_back(A(4, d, a, b, cc, 29)); break; // ps_madd
	case 25: c.push_back(A(4, d, a, b, cc, 28)); break; // ps_msub
	case 26: c.push_back(A(4, d, a, b, cc, 31)); break; // ps_nmadd
	case 27: c.push_back(A(4, d, a, b, cc, 30)); break; // ps_nmsub
	case 28: c.push_back(A(4, d, a, b, cc, 10)); break; // ps_sum0
	case 29: c.push_back(A(4, d, a, b, cc, 11)); break; // ps_sum1
	case 30: c.push_back(A(4, d, a, 0, cc, 12)); break; // ps_muls0
	case 31: c.push_back(A(4, d, a, 0, cc, 13)); break; // ps_muls1
	case 32: c.push_back(A(4, d, a, b, cc, 14)); break; // ps_madds0
	case 33: c.push_back(A(4, d, a, b, cc, 15)); break; // ps_madds1
	case 34: c.push_back(A(4, d, a, b, cc, 23)); break; // ps_sel
	case 35: c.push_back(X(4, d, a, b, 528 + 32 * rnd(4))); break; // ps_merge00/01/10/11
	case 36: c.push_back(X(4, d, 0, b, rnd(3) == 0 ? 40 : rnd(2) ? 72 : 264)); break; // ps_neg / ps_mr / ps_abs
	case 37: c.push_back(X(4, rnd(8) << 2, a, b, rnd(2) ? 0 : 64)); break; // ps_cmpu0 / ps_cmpu1
	// loads and stores (r3 = data)
	case 38: c.push_back(D(48, d, 3, rnd(256) * 4)); break; // lfs
	case 39: c.push_back(D(50, d, 3, rnd(128) * 8)); break; // lfd
	case 40: c.push_back(D(52, d, 3, rnd(256) * 4)); break; // stfs
	case 41: c.push_back(D(54, d, 3, rnd(128) * 8)); break; // stfd
	case 42: c.push_back(PSQ(56, d, 3, rnd(2), rnd(3) ? 0 : rnd(8), rnd(128) * 8)); break; // psq_l
	case 43: c.push_back(PSQ(60, d, 3, rnd(2), rnd(3) ? 0 : rnd(8), rnd(128) * 8)); break; // psq_st
	case 44: c.push_back(A(59, d, a, 0, cc, 25)); break; // fmuls (again: common)
	case 45: c.push_back(A(4, d, a, b, cc, 29)); break; // ps_madd (again)
	// ordered compares (recompiled since 2026-10-23; only asked for with JT_OPS, so the default programs stay as they were)
	case 67: c.push_back(X(63, rnd(8) << 2, a, b, 32)); break; // fcmpo
	case 68: c.push_back(X(4, rnd(8) << 2, a, b, 32)); break; // ps_cmpo0
	default:
	{
		const uint32 rd = 6 + rnd(8), ra = 6 + rnd(8), rb = 6 + rnd(8), rc = rnd(2);
		auto xo = [&](uint32 x) { return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (x << 1) | rc; };
		auto xo1 = [&](uint32 x) { return (31u << 26) | (rd << 21) | (ra << 16) | (x << 1) | rc; }; // no rB
		const uint32 simm = rnd(4) ? rnd(0x10000) : (rnd(2) ? 0xFFFF : 0);
		switch (s_lastKind)
		{
		case 46: c.push_back(xo(10)); break; // addc
		case 47: c.push_back(xo(138)); break; // adde
		case 48: c.push_back(xo1(202)); break; // addze
		case 49: c.push_back(xo1(234)); break; // addme
		case 50: c.push_back(xo(8)); break; // subfc
		case 51: c.push_back(xo(136)); break; // subfe
		case 52: c.push_back(xo1(200)); break; // subfze
		case 53: c.push_back(xo1(232)); break; // subfme
		case 54: c.push_back(xo1(104)); break; // neg
		case 55: c.push_back(D(8, rd, ra, simm)); break; // subfic
		case 56: c.push_back(D(rnd(2) ? 12 : 13, rd, ra, simm)); break; // addic / addic.
		case 57: c.push_back((31u << 26) | (ra << 21) | (rd << 16) | (rnd(32) << 11) | (824 << 1) | rc); break; // srawi rd, ra
		case 58: c.push_back((31u << 26) | (ra << 21) | (rd << 16) | (rb << 11) | (792 << 1) | rc); break; // sraw rd, ra, rb
		case 59: c.push_back(xo(75)); break; // mulhw
		case 60: c.push_back(xo(11)); break; // mulhwu
		case 61: c.push_back(xo(235)); break; // mullw
		case 62: c.push_back(xo(rnd(2) ? 491 : 459)); break; // divw / divwu
		case 63: c.push_back((31u << 26) | ((rnd(8) << 2) << 21) | (ra << 16) | (rb << 11) | ((rnd(2) ? 0 : 32) << 1)); break; // cmpw / cmplw
		case 64: c.push_back((21u << 26) | (ra << 21) | (rd << 16) | (rnd(32) << 11) | (rnd(32) << 6) | (rnd(32) << 1) | rc); break; // rlwinm
		case 65: c.push_back(xo(40)); break; // subf
		case 66: c.push_back(D(14, rd, ra, simm)); break; // addi
		}
	}
	}
}

// one instruction, or a compare and a forward branch over a few (several segments); returns the
// words emitted
static int emitGroup(Program& p, int room, bool inLoop)
{
	if (!s_noBranch && rnd(12) == 0 && room > 4)
	{
		const uint32 crf = 1 + rnd(3);
		p.code.push_back(X(63, crf << 2, fr(), fr(), 0)); // fcmpu crf
		const uint32 skip = 1 + rnd(3);
		const uint32 bo = rnd(2) ? 12 : 4; // branch if true / false
		const uint32 bi = crf * 4 + rnd(3);
		p.code.push_back((16u << 26) | (bo << 21) | (bi << 16) | (((skip + 1) * 4) & 0xFFFC));
		for (uint32 k = 0; k < skip; k++)
		{
			if (inLoop)
				p.entryCandidates.emplace_back((uint32)p.code.size(), kEntryInLoop);
			emitOne(p.code);
		}
		p.entryCandidates.emplace_back((uint32)p.code.size(), kEntryBranchTarget);
		return 2 + (int)skip;
	}
	if (inLoop)
		p.entryCandidates.emplace_back((uint32)p.code.size(), kEntryInLoop);
	emitOne(p.code);
	return 1;
}

static Program makeProgram(int length)
{
	Program p;
	for (int i = 0; i < length;)
	{
		// sometimes a loop of 1-3 rounds: li r15 / mtctr ... bdnz, or li r16 ... addic. r16 / bne
		// (entered in the middle, it runs as often as the entry state's CTR / r16 say: 1-3)
		if (s_loops && rnd(6) == 0 && i + 10 < length)
		{
			const bool ctr = rnd(2) != 0;
			const uint32 rounds = 1 + rnd(3);
			if (ctr)
			{
				p.code.push_back(D(14, 15, 0, rounds)); // li r15
				p.code.push_back(0x7DE903A6); // mtctr r15
			}
			else
				p.code.push_back(D(14, 16, 0, rounds)); // li r16
			const uint32 head = (uint32)p.code.size();
			p.entryCandidates.emplace_back(head, kEntryLoopHead);
			const int bodyLength = 2 + (int)rnd(8);
			for (int b = 0; b < bodyLength;)
				b += emitGroup(p, bodyLength - b, true);
			if (!ctr)
			{
				p.entryCandidates.emplace_back((uint32)p.code.size(), kEntryInLoop);
				p.code.push_back(D(13, 16, 16, 0xFFFF)); // addic. r16, r16, -1
			}
			p.entryCandidates.emplace_back((uint32)p.code.size(), kEntryInLoop);
			const sint32 back = ((sint32)head - (sint32)p.code.size()) * 4;
			if (ctr)
				p.code.push_back((16u << 26) | (16u << 21) | ((uint32)back & 0xFFFC)); // bdnz head
			else
				p.code.push_back((16u << 26) | (4u << 21) | (2u << 16) | ((uint32)back & 0xFFFC)); // bne cr0, head
			p.entryCandidates.emplace_back((uint32)p.code.size(), kEntryBranchTarget); // after the loop
			i = (int)p.code.size();
			continue;
		}
		i += emitGroup(p, length - i, false);
	}
	for (uint32 w = 1; w < (uint32)p.code.size(); w++)
		p.entryCandidates.emplace_back(w, kEntryAny);
	p.code.push_back(0x4E800020); // blr
	return p;
}

static bool s_singlesOnly() { return s_singles; }

struct State
{
	PPCInterpreter_t cpu;
	std::vector<uint8> data;
};

static PPCInterpreterGlobal_t* s_global;

static void initState(State& s)
{
	memset(&s.cpu, 0, sizeof(s.cpu));
	s.cpu.global = s_global;
	s.cpu.LSQE = 1;
	s.cpu.PSE = 1;
	s.cpu.remainingCycles = 1 << 30;
	for (int i = 0; i < 32; i++)
	{
		const uint32 special[] = {0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFF, 0x10000};
		s.cpu.gpr[i] = rnd(4) ? (uint32)rng() : special[rnd(7)];
	}
	s.cpu.xer_ca = rnd(2);
	s.cpu.xer_so = getenv("JT_SO") ? rnd(2) : 0; // the recompiler ignores SO in CR0 (as Cemu)
	s.cpu.gpr[1] = kData + kDataSize - 0x100;
	s.cpu.gpr[3] = kData + 0x100;
	if (s_loops || s_entries)
	{
		// a loop entered in the middle (or at its mtctr) runs as often as these say
		s.cpu.spr.CTR = 1 + rnd(3);
		s.cpu.gpr[15] = 1 + rnd(3);
		s.cpu.gpr[16] = 1 + rnd(3);
		// a recompiled loop that never ends leaves at a cycle check instead of hanging
		s.cpu.remainingCycles = 1 << 22;
	}
	for (int i = 0; i < 32; i++)
	{
		s.cpu.fpr[i].fp0 = randomValue();
		s.cpu.fpr[i].fp1 = rnd(3) || s_singles ? randomSingle() : randomValue();
	}
	for (int i = 0; i < 32; i++)
		s.cpu.cr[i] = (i & 3) == 3 && !getenv("JT_SO") ? 0 : rnd(2); // SO bits as XER.SO (0): the recompiler leaves them
	s.cpu.spr.LR = kReturn;
	// GQR0 float (the recompiler assumes so), GQR1-7 a mix of float and scaled integer types
	s.cpu.spr.UGQR[0] = 0;
	for (int i = 1; i < 8; i++)
	{
		const uint32 types[] = {0, 0, 4, 5, 6, 7};
		const uint32 ld = types[rnd(6)], st = types[rnd(6)];
		const uint32 lscale = ld ? rnd(8) : 0, sscale = st ? rnd(8) : 0;
		s.cpu.spr.UGQR[i] = (lscale << 24) | (ld << 16) | (sscale << 8) | st;
	}
	s.data.resize(kDataSize);
	for (uint32 i = 0; i < kDataSize; i += 4)
	{
		uint32 v;
		if (rnd(4))
		{
			const float f = (float)randomSingle();
			memcpy(&v, &f, 4);
		}
		else
			v = (uint32)rng();
		v = _swapEndianU32(v);
		memcpy(&s.data[i], &v, 4);
	}
}

static uint64 hashState(const PPCInterpreter_t& cpu, const uint8* data)
{
	uint64 h = 1469598103934665603ull;
	auto mix = [&](const void* p, size_t n) {
		const uint8* b = (const uint8*)p;
		for (size_t i = 0; i < n; i++)
			h = (h ^ b[i]) * 1099511628211ull;
	};
	mix(cpu.gpr, sizeof(cpu.gpr));
	for (int i = 0; i < 32; i++)
	{
		// NaNs hash alike (payloads aside)
		for (double v : {cpu.fpr[i].fp0, cpu.fpr[i].fp1})
		{
			if (v != v)
				mix("nan", 3);
			else
				mix(&v, 8);
		}
	}
	// every run ends with a return: the recompiled code leaves the EABI's volatile CR fields (cr0, cr1,
	// cr5-cr7) as they come with g_crema_eabi_returns, only cr2-cr4 reach the caller
	uint8 cr[32];
	memcpy(cr, cpu.cr, sizeof(cr));
	if (g_crema_eabi_returns)
		for (int i = 0; i < 32; i++)
			if (i < 8 || i >= 20)
				cr[i] = 0;
	mix(cr, sizeof(cr));
	mix(&cpu.xer_ca, 1);
	mix(&cpu.xer_so, 1);
	mix(&cpu.instructionPointer, 4);
	mix(&cpu.spr.CTR, 4);
	mix(data, kDataSize);
	return h;
}

static void runInterpreter(PPCInterpreter_t* cpu, uint32 start = kCode)
{
	PPCInterpreter_setCurrentInstance(cpu);
	cpu->instructionPointer = start;
	for (int steps = 0; steps < 100000 && cpu->instructionPointer != kReturn; steps++)
		PPCInterpreterSlim_executeInstruction(cpu);
}

static uint64 s_codeBytes = 0, s_benchTicks = 0, s_benchRuns = 0;
static int s_bench = 0;

// the program compiled once with all its entries (kCode first); its range and the boundary
// tracker's ranges for the compiled code cache
static PPCRecFunction_t* compileProgram(const std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPoints,
	PPCFunctionBoundaryTracker::PPCRange_t& range, std::vector<PPCFunctionBoundaryTracker::PPCRange_t>& tracked)
{
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(kCode);
	if (!funcBoundaries.getRangeForAddress(kCode, range))
		return nullptr;
	tracked = funcBoundaries.GetRanges();
	std::set<uint32> entries = entryAddresses;
	return PPCRecompiler_recompileFunction(range, entries, entryPoints, funcBoundaries);
}

// JT_CACHE=file: each program goes through the compiled code cache (PPCRecompiler.cpp) - recorded,
// written, its jump table entries forgotten, loaded again (its x86 code copied elsewhere) - and that
// copy runs; the compile's own code becomes int3, so nothing runs it by mistake. Each program's
// code is also changed in one instruction and loaded: the record must be left out.
static const char* s_cachePath = nullptr;
static int s_cacheFailures = 0;

static bool throughCache(PPCRecFunction_t* func, const std::vector<std::pair<MPTR, uint32>>& entryPoints, const PPCFunctionBoundaryTracker::PPCRange_t& range,
	const std::vector<PPCFunctionBoundaryTracker::PPCRange_t>& tracked, uint32 codeWords, int t)
{
	uint32 span = range.length;
	for (const auto& r : tracked)
		span = std::max(span, r.startAddress + r.length - kCode);
	span = std::max(span, codeWords * 4) + 0x100;
	uint32 counts[5];
	auto load = [&](uint32 wantActivated, uint32 wantChanged, const char* what) {
		CremaCodeTestForget(kCode, span);
		CremaCodeTestReset();
		CremaCodeTestLoad(s_cachePath, counts);
		if (counts[0] == wantActivated && counts[2] == wantChanged && counts[3] == 0 && counts[4] == 0)
			return true;
		printf("%d cache (%s): %u activated, %u compiled meanwhile, %u changed, %u invalid, %u twice\n", t, what, counts[0], counts[1], counts[2],
			counts[3], counts[4]);
		return false;
	};
	CremaCodeTestReset();
	remove(s_cachePath);
	CremaCodeTestRecord(kCode, range, tracked, func, entryPoints);
	CremaCodeTestWrite(s_cachePath, true);
	// one instruction of the function changed: left out
	uint32* word = (uint32*)(memory_base + kCode) + (uint32)t % (range.length / 4);
	const uint32 original = *word;
	*word ^= _swapEndianU32(0x00200000); // another register
	const bool changedOk = load(0, 1, "changed code");
	*word = original;
	if (!load(1, 0, "same code") || !changedOk)
		return false;
	auto* table = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
	uint32 kCodeOffset = 0xFFFFFFFF;
	for (auto& [ppc, x86] : entryPoints)
		if (ppc == kCode)
			kCodeOffset = x86;
	if (kCodeOffset == 0xFFFFFFFF)
		return false;
	uint8* copy = (uint8*)table[kCode / 4] - kCodeOffset;
	for (auto& [ppc, x86] : entryPoints)
		if ((uint8*)table[ppc / 4] != copy + x86)
		{
			printf("%d cache: entry %08x at %p, not %p\n", t, ppc, (void*)table[ppc / 4], (void*)(copy + x86));
			return false;
		}
	if (copy == func->x86Code || memcmp(copy, func->x86Code, func->x86Size) != 0)
		return false;
	memset(func->x86Code, 0xCC, func->x86Size);
	func->x86Code = copy;
	return true;
}

static bool runRecompiled(PPCInterpreter_t* cpu, PPCRecFunction_t* func, const std::vector<std::pair<MPTR, uint32>>& entryPoints, uint32 start)
{
	PPCInterpreter_setCurrentInstance(cpu);
	uint8* entry = nullptr;
	for (auto& [ppc, x86] : entryPoints)
		if (ppc == start)
			entry = (uint8*)func->x86Code + x86;
	if (!entry)
		return false;
	cpu->instructionPointer = start;
	// the entry takes (code, hCPU) in RCX/RDX: a SysV call with them as the 4th and 3rd argument
	// (SysV treats every xmm register as clobbered, which the generated code does)
	auto enter = (void (*)(uint64, uint64, uint64, uint64))PPCRecompiler_enterRecompilerCode;
	if (s_bench && start == kCode)
	{
		PPCInterpreter_t* copy = (PPCInterpreter_t*)aligned_alloc(64, (sizeof(PPCInterpreter_t) + 63) & ~63);
		memcpy(copy, cpu, sizeof(PPCInterpreter_t));
		const uint64 benchStart = __rdtsc();
		for (int i = 0; i < s_bench; i++)
		{
			memcpy(copy, cpu, sizeof(PPCInterpreter_t));
			enter(0, 0, (uint64)copy, (uint64)entry);
		}
		s_benchTicks += __rdtsc() - benchStart;
		s_benchRuns += s_bench;
		free(copy);
	}
	enter(0, 0, (uint64)cpu, (uint64)entry);
	return true;
}

static void describe(const char* what, const PPCInterpreter_t& x, const PPCInterpreter_t& y, const uint8* dx, const uint8* dy)
{
	int shown = 0;
	for (int i = 0; i < 32 && shown < 8; i++)
	{
		for (int lane = 0; lane < 2; lane++)
		{
			const double a = lane ? x.fpr[i].fp1 : x.fpr[i].fp0, b = lane ? y.fpr[i].fp1 : y.fpr[i].fp0;
			uint64 ba, bb;
			memcpy(&ba, &a, 8);
			memcpy(&bb, &b, 8);
			if (ba != bb && !(a != a && b != b))
			{
				printf("    %s f%d.ps%d %.17g (%016llx) vs %.17g (%016llx)\n", what, i, lane, a, (unsigned long long)ba, b, (unsigned long long)bb);
				shown++;
			}
		}
	}
	for (int i = 0; i < 32; i++)
		if (x.gpr[i] != y.gpr[i])
			printf("    %s r%d %08x vs %08x\n", what, i, x.gpr[i], y.gpr[i]);
	for (int i = 0; i < 32; i++)
		if (x.cr[i] != y.cr[i])
			printf("    %s cr bit %d %d vs %d\n", what, i, x.cr[i], y.cr[i]);
	if (x.xer_ca != y.xer_ca)
		printf("    %s xer.ca %d vs %d\n", what, x.xer_ca, y.xer_ca);
	if (x.instructionPointer != y.instructionPointer)
		printf("    %s ip %08x vs %08x\n", what, x.instructionPointer, y.instructionPointer);
	int memShown = 0;
	for (uint32 i = 0; i < kDataSize && memShown < 4; i += 4)
		if (memcmp(dx + i, dy + i, 4) != 0)
		{
			printf("    %s mem +%04x %08x vs %08x\n", what, i, _swapEndianU32(*(uint32*)(dx + i)), _swapEndianU32(*(uint32*)(dy + i)));
			memShown++;
		}
}

static void disassemble(const Program& p)
{
	for (size_t i = 0; i < p.code.size(); i++)
		printf("      %08x: %08x\n", kCode + (uint32)i * 4, p.code[i]);
}

// ahead-of-time compiles (BackendC): the C++ of the compile running now (PPCRecompiler.cpp calls the hook
// before register allocation)
bool (*g_jtAotHook)(ppcImlGenContext_t& ctx) = nullptr; // PPCRecompiler.cpp
static AotFunctionOut* s_aotOut = nullptr;
static std::string s_aotName, s_aotWhy;
static bool s_aotOk = false;

static bool aotHook(ppcImlGenContext_t& ctx)
{
	s_aotOk = PPCRecompiler_generateCCode(ctx, s_aotName, *s_aotOut, s_aotWhy);
	return s_aotOk;
}

// JT_AOT_REAL=file.aot (with JT_RPX): each function the JIT compiles also runs, from random states,
// as the JIT's x86 code and as the translated code file's function (aotbuild.sh), until it leaves
// its code; the two results must be the same (CPU state, cycles, memory). Guest memory: the game's
// code (read only), zeros elsewhere (read only), a scratch area whose words mostly point into it
// (written), so the functions' pointers mostly land there; a run that faults on both sides is
// left out. JT_AOT_RUNS: runs per function (2: from its start, then from another entry).
#include "Cafe/HW/Espresso/Recompiler/BackendC/AotFormat.h"
#include <setjmp.h>
#include <sys/time.h>
constexpr uint32 kRealData = 0x10000000;
constexpr uint32 kRealSize = 0x200000;

struct RealAotTest
{
	const CremaAotManifest* manifest = nullptr;
	std::unordered_map<uint32, uint32> byEntry; // record index by its function's address
	std::vector<uint8> pristine;
	std::vector<uint8> jitData;
	uint32 codeStart = 0, codeEnd = 0;
	int runs = 2;
	uint64 functions = 0, missing = 0, entryMismatch = 0, compared = 0, bothFaulted = 0, faultMismatch = 0, differ = 0, nanOnly = 0, tableFaults = 0;
	uint64 jitTicks = 0, aotTicks = 0;
	int reported = 0;
};

static sigjmp_buf s_realJump;
static volatile sig_atomic_t s_realCatch = 0;
static void* volatile s_realFaultAddress = nullptr;
static volatile uint64 s_realFaultRip = 0;
static void onCrash(int sig, siginfo_t* info, void* context);

// JT_AOT_CHAIN: the scratch area is write-protected; the first write to each page marks it dirty
// (and makes it writable), so a run restores only what the run before it changed: a 2 MiB copy
// before each run pushed the code out of the caches it was timed in
constexpr uint32 kRealPages = 0x200000 / 4096;
static volatile sig_atomic_t s_pageTracking = 0;
static uint8 s_pageDirty[kRealPages];
static uint32 s_dirtyList[kRealPages];
static volatile uint32 s_dirtyCount = 0;

static void onRealFault(int sig, siginfo_t* info, void* context)
{
	if (s_pageTracking && sig == SIGSEGV)
	{
		const uint8* scratch = memory_base + 0x10000000;
		const uint8* at = (const uint8*)info->si_addr;
		if (at >= scratch && at < scratch + 0x200000)
		{
			const uint32 page = (uint32)((at - scratch) / 4096);
			if (!s_pageDirty[page])
			{
				s_pageDirty[page] = 1;
				s_dirtyList[s_dirtyCount++] = page;
				mprotect((void*)(scratch + page * 4096), 4096, PROT_READ | PROT_WRITE);
				return; // the write runs again
			}
		}
	}
	if (s_realCatch)
	{
		s_realCatch = 0;
		s_realFaultAddress = info->si_addr;
		s_realFaultRip = (uint64)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
		siglongjmp(s_realJump, 1);
	}
	onCrash(sig, info, context);
}

static uint32 realPointer()
{
	return kRealData + 0x8000 + (rnd(kRealSize - 0x10000) & ~0xFu);
}

// the CPU state real functions run on: the JIT's code calls its helpers (fres, frsqrte, the time
// base) with RSP still at hCPU, so their stack frames go below it - it needs room there (a local
// had the harness's own stack overwritten: garbage return addresses)
static PPCInterpreter_t& runCpu()
{
	static uint8* area = (uint8*)aligned_alloc(64, 0x20000);
	return *(PPCInterpreter_t*)(area + 0x10000);
}

static void initRealState(PPCInterpreter_t& cpu, const RealAotTest& t)
{
	memset(&cpu, 0, sizeof(cpu));
	cpu.global = s_global;
	cpu.LSQE = 1;
	cpu.PSE = 1;
	cpu.remainingCycles = 20000;
	for (int i = 0; i < 32; i++)
	{
		const uint32 special[] = {0, 1, 2, 4, 0xFFFFFFFF, 0x80000000, 0x10, 0x100};
		cpu.gpr[i] = rnd(8) ? realPointer() : (rnd(2) ? special[rnd(8)] : (uint32)rng());
	}
	cpu.gpr[1] = kRealData + kRealSize / 2;
	cpu.gpr[2] = kRealData + 0x10000;
	cpu.gpr[13] = kRealData + 0x20000;
	for (int i = 0; i < 32; i++)
	{
		cpu.fpr[i].fp0 = randomValue();
		cpu.fpr[i].fp1 = rnd(3) ? randomSingle() : randomValue();
	}
	for (int i = 0; i < 32; i++)
		cpu.cr[i] = (i & 3) == 3 ? 0 : rnd(2);
	cpu.xer_ca = rnd(2);
	cpu.spr.LR = t.codeStart + (rnd((t.codeEnd - t.codeStart) / 4) << 2);
	cpu.spr.CTR = t.codeStart + (rnd((t.codeEnd - t.codeStart) / 4) << 2);
	cpu.spr.UGQR[0] = 0;
	for (int i = 1; i < 8; i++)
	{
		const uint32 types[] = {0, 0, 4, 5, 6, 7};
		const uint32 ld = types[rnd(6)], st = types[rnd(6)];
		cpu.spr.UGQR[i] = ((ld ? rnd(8) : 0) << 24) | (ld << 16) | ((st ? rnd(8) : 0) << 8) | st;
	}
}

// the parts of the CPU state the two may write, compared (NaNs alike in float registers: their sign
// and payload depend on operand order); true if the same
static bool sameRealState(const PPCInterpreter_t& x, const PPCInterpreter_t& y, bool& nanOnly, std::string& what)
{
	nanOnly = false;
	char text[160];
	for (int i = 0; i < 32; i++)
		if (x.gpr[i] != y.gpr[i])
		{
			snprintf(text, sizeof(text), "r%d %08x %08x", i, x.gpr[i], y.gpr[i]);
			what = text;
			return false;
		}
	for (int i = 0; i < 32; i++)
		if (x.cr[i] != y.cr[i])
		{
			snprintf(text, sizeof(text), "cr bit %d", i);
			what = text;
			return false;
		}
	const struct { const char* name; uint32 a, b; } words[] = {
		{"ca", x.xer_ca, y.xer_ca}, {"so", x.xer_so, y.xer_so}, {"lr", x.spr.LR, y.spr.LR}, {"ctr", x.spr.CTR, y.spr.CTR}, {"xer", x.spr.XER, y.spr.XER},
		{"ip", x.instructionPointer, y.instructionPointer}, {"cycles", (uint32)x.remainingCycles, (uint32)y.remainingCycles},
		{"reservation", x.reservedMemAddr, y.reservedMemAddr}, {"reserved value", x.reservedMemValue, y.reservedMemValue}};
	for (auto& w : words)
		if (w.a != w.b)
		{
			snprintf(text, sizeof(text), "%s %08x %08x", w.name, w.a, w.b);
			what = text;
			return false;
		}
	for (int i = 0; i < 8; i++)
		if (x.spr.UGQR[i] != y.spr.UGQR[i])
		{
			what = "gqr";
			return false;
		}
	for (int i = 0; i < 32; i++)
		for (int lane = 0; lane < 2; lane++)
		{
			const double a = lane ? x.fpr[i].fp1 : x.fpr[i].fp0, b = lane ? y.fpr[i].fp1 : y.fpr[i].fp0;
			if (memcmp(&a, &b, 8) == 0)
				continue;
			if (a != a && b != b)
			{
				nanOnly = true;
				continue;
			}
			snprintf(text, sizeof(text), "f%d.%d %016llx %016llx", i, lane, (unsigned long long)*(const uint64*)&a, (unsigned long long)*(const uint64*)&b);
			what = text;
			return false;
		}
	return true;
}

static void runRealAot(RealAotTest& t, uint32 address, PPCRecFunction_t* func, const std::vector<std::pair<MPTR, uint32>>& entryPoints)
{
	auto found = t.byEntry.find(address);
	if (found == t.byEntry.end())
	{
		t.missing++;
		return;
	}
	t.functions++;
	const CremaAotRecord& rec = t.manifest->records[found->second];
	const uint32* entries = t.manifest->pairs + rec.firstPair + rec.hashRanges * 2;
	std::vector<uint32> aotEntries(entries, entries + rec.entryPoints), jitEntries;
	for (auto& [ppc, x86] : entryPoints)
		jitEntries.push_back(ppc);
	std::sort(aotEntries.begin(), aotEntries.end());
	std::sort(jitEntries.begin(), jitEntries.end());
	if (aotEntries != jitEntries)
		t.entryMismatch++;
	const auto fn = t.manifest->functions[found->second];
	auto enter = (void (*)(uint64, uint64, uint64, uint64))PPCRecompiler_enterRecompilerCode;
	uint8* scratch = memory_base + kRealData;
	for (int r = 0; r < t.runs; r++)
	{
		uint32 start = address;
		if (r > 0 && !jitEntries.empty())
			start = jitEntries[rnd((uint32)jitEntries.size())];
		if (!std::binary_search(aotEntries.begin(), aotEntries.end(), start))
			continue;
		uint8* jitEntry = nullptr;
		for (auto& [ppc, x86] : entryPoints)
			if (ppc == start)
				jitEntry = (uint8*)func->x86Code + x86;
		PPCInterpreter_t initial;
		initRealState(initial, t);
		// JT_AOT_ONLY=address: only that function's runs (the random states as in a full run)
		static const uint32 only = getenv("JT_AOT_ONLY") ? (uint32)strtoul(getenv("JT_AOT_ONLY"), nullptr, 16) : 0;
		if (only && address != only)
			continue;
		// the JIT's run
		PPCInterpreter_t& cpu = runCpu();
		cpu = initial;
		memcpy(scratch, t.pristine.data(), kRealSize);
		PPCInterpreter_setCurrentInstance(&cpu);
		cpu.instructionPointer = start;
		_mm_setcsr(0x9FC0);
		bool jitFaulted = false;
		uint64 t0 = __rdtsc();
		if (sigsetjmp(s_realJump, 1) == 0)
		{
			s_realCatch = 1;
			enter(0, 0, (uint64)&cpu, (uint64)jitEntry);
			s_realCatch = 0;
		}
		else
			jitFaulted = true;
		const uint64 jitTime = __rdtsc() - t0;
		PPCInterpreter_t jit = cpu;
		memcpy(t.jitData.data(), scratch, kRealSize);
		// the translated function's
		cpu = initial;
		memcpy(scratch, t.pristine.data(), kRealSize);
		PPCInterpreter_setCurrentInstance(&cpu);
		cpu.instructionPointer = start;
		_mm_setcsr(0x9FC0);
		bool aotFaulted = false;
		// only this function may run (its flag set): the ones it calls by name return at once, so the
		// run stops where it leaves its code, as the JIT's does
		uint8* const flag = t.manifest->flags ? t.manifest->flags[found->second] : nullptr;
		if (flag)
			*flag = 1;
		// (the host stack the function's depth check measures from, as the JIT's entry sets it)
		cpu.rspTemp = __builtin_frame_address(0);
		t0 = __rdtsc();
		if (sigsetjmp(s_realJump, 1) == 0)
		{
			s_realCatch = 1;
			const uint32 next = fn((uint8*)&cpu, memory_base, start);
			s_realCatch = 0;
			cpu.instructionPointer = next & ~1u;
		}
		else
			aotFaulted = true;
		const uint64 aotTime = __rdtsc() - t0;
		if (flag)
			*flag = 0;
		PPCInterpreter_t aot = cpu;
		_mm_setcsr(0x9FC0);
		if (jitFaulted && aotFaulted)
		{
			t.bothFaulted++;
			continue;
		}
		std::string what;
		bool nanOnly = false;
		bool same;
		// the translated function returns where execution goes on; the JIT's code jumps there through
		// the jump table (at the address * 2), which faults where no code was ever reserved (a garbage
		// branch target), or jumps to 0 from a slot the test's table has as zeros: the same exit, with
		// the guest registers stored, only the cycles not written back
		// (a computed branch's low bits: the JIT's slot is at the raw address * 2, translated code
		// ignores them as the CPU does)
		const uint8* table = (const uint8*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
		const bool inTable = (const uint8*)s_realFaultAddress >= table && (const uint8*)s_realFaultAddress < table + 0x200000000ull;
		const bool tableFault = jitFaulted && !aotFaulted && (aot.instructionPointer & 1) == 0 && (inTable || s_realFaultRip == 0);
		if (tableFault)
		{
			t.tableFaults++;
			jit.instructionPointer = aot.instructionPointer;
			jit.remainingCycles = aot.remainingCycles;
			same = sameRealState(aot, jit, nanOnly, what);
			if (same && memcmp(scratch, t.jitData.data(), kRealSize) != 0)
			{
				same = false;
				what = "memory (before a jump through an unreserved jump table block)";
			}
		}
		else if (jitFaulted != aotFaulted)
		{
			t.faultMismatch++;
			same = false;
			char text[120];
			snprintf(text, sizeof(text), "only the %s run faulted (at %p; translated exit %08x)", jitFaulted ? "JIT's" : "translated", s_realFaultAddress,
				aot.instructionPointer);
			what = text;
		}
		else
		{
			t.compared++;
			t.jitTicks += jitTime;
			t.aotTicks += aotTime;
			same = sameRealState(aot, jit, nanOnly, what);
			if (same && memcmp(scratch, t.jitData.data(), kRealSize) != 0)
			{
				// memory: NaN floats alike
				auto isNan = [](uint32 w) { return ((w >> 23) & 0xFF) == 0xFF && (w & 0x7FFFFF) != 0; };
				for (uint32 i = 0; i < kRealSize && same; i += 4)
				{
					const uint32 a = _swapEndianU32(*(const uint32*)(scratch + i)), b = _swapEndianU32(*(const uint32*)(t.jitData.data() + i));
					if (a == b)
						continue;
					if (isNan(a) && isNan(b))
						nanOnly = true;
					else
					{
						char text[80];
						snprintf(text, sizeof(text), "memory %08x: %08x (translated) %08x (jit)", kRealData + i, a, b);
						what = text;
						same = false;
					}
				}
			}
			if (same && nanOnly)
				t.nanOnly++;
		}
		if (!same)
		{
			t.differ++;
			// (JT_REPORT=n: how many to print, 60)
			static const int reportLimit = getenv("JT_REPORT") ? atoi(getenv("JT_REPORT")) : 60;
			if (t.reported++ < reportLimit)
				printf("REAL DIFF function %08x from %08x: %s\n", address, start, what.c_str());
			if (only)
			{
				// where the JIT's code faulted, and the two final states side by side
				if (jitFaulted)
					printf("  JIT fault at x86 +0x%llx (rip %llx, address %p), x86 code %p size %u\n", (unsigned long long)(s_realFaultRip - (uint64)func->x86Code),
						(unsigned long long)s_realFaultRip, s_realFaultAddress, func->x86Code, func->x86Size);
				for (int i = 0; i < 32; i++)
					if (aot.gpr[i] != jit.gpr[i] || aot.gpr[i] != initial.gpr[i])
						printf("  r%-2d initial %08x translated %08x jit %08x\n", i, initial.gpr[i], aot.gpr[i], jit.gpr[i]);
				printf("  lr %08x/%08x ctr %08x/%08x ip %08x/%08x cycles %d/%d\n", aot.spr.LR, jit.spr.LR, aot.spr.CTR, jit.spr.CTR, aot.instructionPointer,
					jit.instructionPointer, aot.remainingCycles, jit.remainingCycles);
				for (uint32 i = 0; i < kRealSize; i += 4)
				{
					const uint32 a = *(const uint32*)(scratch + i), b = *(const uint32*)(t.jitData.data() + i), p = *(const uint32*)(t.pristine.data() + i);
					if (a != b)
						printf("  memory %08x: initial %08x translated %08x jit %08x\n", kRealData + i, _swapEndianU32(p), _swapEndianU32(a), _swapEndianU32(b));
				}
				if (getenv("JT_CODE"))
					writeFile(getenv("JT_CODE"), std::string((const char*)func->x86Code, func->x86Size));
			}
		}
	}
}

// JT_AOT_CHAIN=file.aot (with JT_RPX): the file loaded as at a game's start (its functions made
// active in the jump table), then real functions run from random states through the jump table as
// on the console - calls, returns and jumps between translated functions included - until control
// leaves translated code (the cycle budget, an address nothing is compiled for): with direct calls
// and with every transition through the bridge (version 1's way), which must give the same results;
// with JT_CACHE=file also the JIT's code of every function (the compiled code cache loaded
// instead), compared and timed. JT_AOT_CHAINS: how many runs (20000).
static uint64 hashScratch(const uint8* p)
{
	uint64 hash = 0x243F6A8885A308D3ull;
	for (uint32 i = 0; i < kRealSize; i += 8)
	{
		uint64 w;
		memcpy(&w, p + i, 8);
		hash = (hash ^ w) * 0x9E3779B97F4A7C15ull;
	}
	return hash;
}

struct ChainRun
{
	uint32 start;
	PPCInterpreter_t initial;
	bool faulted[3];
	bool timedOut[3] = {};
	bool ended[3] = {}; // EABI calls: stopped inside translated code (see onAotTestEnd)
	uint64 hash[3];
	uint64 ticks[3];
};

// JT_AOT_ABI=1 (a file made with EABI calls): translated code never leaves through the jump table
// in the middle of a call (its callers' non-volatile registers are in their host frames, not in
// hCPU); where the JIT's code would leave - a time slice over, an address nothing is translated
// for, an instruction for the interpreter - it calls its imports, which here end the run (the JIT's
// run ends there too). Such a run's state can't be compared, its time can. The functions' saves of
// those registers on the guest stack hold what hCPU had (their callers' are in host frames), so the
// stack's pages aren't compared either.
static bool s_aotAbi = false;
extern void (*g_cremaAotTestEnd)(int why); // PPCRecompiler.cpp
static volatile sig_atomic_t s_aotEnded = 0;

static void onAotTestEnd(int why)
{
	if (!s_realCatch)
		return;
	s_realCatch = 0;
	s_aotEnded = why;
	siglongjmp(s_realJump, 2);
}

// a chain that never leaves (random states make functions jump to each other in a ring: no cycle
// check on the way, on a Wii U it would spin too) is stopped after 50 ms, as a fault
static volatile sig_atomic_t s_realTimedOut = 0;

static void onRealTimeout(int sig, siginfo_t* info, void* context)
{
	if (s_realCatch)
	{
		s_realCatch = 0;
		s_realTimedOut = 1;
		s_realFaultAddress = nullptr;
		s_realFaultRip = (uint64)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
		siglongjmp(s_realJump, 1);
	}
}

// the pages the runs before dirtied copied back from the pristine scratch (and with protect,
// write-protected again: the next run's writes found afresh)
static void restoreDirtyPages(const RealAotTest& t, bool protect)
{
	uint8* scratch = memory_base + kRealData;
	for (uint32 i = 0; i < s_dirtyCount; i++)
	{
		const uint32 page = s_dirtyList[i];
		memcpy(scratch + page * 4096, t.pristine.data() + page * 4096, 4096);
		if (protect)
		{
			mprotect(scratch + page * 4096, 4096, PROT_READ);
			s_pageDirty[page] = 0;
		}
	}
	if (protect)
		s_dirtyCount = 0;
}

// the pages written, in address order, with what is in them (words that are float NaNs all alike:
// a NaN's sign and payload depend on operand order, as in sameRealState)
static uint64 hashDirtyPages()
{
	std::vector<uint32> pages(s_dirtyList, s_dirtyList + s_dirtyCount);
	std::sort(pages.begin(), pages.end());
	if (s_aotAbi)
	{
		// (the guest stack, below r1's start: see s_aotAbi)
		const uint32 stackTop = (kRealSize / 2) / 4096;
		pages.erase(std::remove_if(pages.begin(), pages.end(), [&](uint32 p) { return p + 16 >= stackTop && p <= stackTop; }), pages.end());
	}
	uint64 hash = 0x243F6A8885A308D3ull ^ pages.size();
	for (uint32 page : pages)
	{
		hash = (hash ^ page) * 0x9E3779B97F4A7C15ull;
		const uint8* p = memory_base + kRealData + page * 4096;
		for (uint32 i = 0; i < 4096; i += 4)
		{
			uint32 w = _swapEndianU32(*(const uint32*)(p + i));
			if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu) != 0)
				w = 0x7FC00000u;
			hash = (hash ^ w) * 0x9E3779B97F4A7C15ull;
		}
	}
	return hash;
}

static bool runChainOnce(const ChainRun& run, PPCInterpreter_t& cpu, bool& timedOut, bool* ended = nullptr)
{
	cpu = run.initial;
	_mm_setcsr(0x9FC0);
	s_realTimedOut = 0;
	s_aotEnded = 0;
	itimerval timer{};
	timer.it_value.tv_usec = 50000;
	bool faulted = false;
	const int jumped = sigsetjmp(s_realJump, 1);
	if (jumped == 0)
	{
		s_realCatch = 1;
		setitimer(ITIMER_REAL, &timer, nullptr);
		faulted = !runThroughTable(&cpu, run.start);
		s_realCatch = 0;
	}
	else if (jumped != 2)
		faulted = true;
	timer.it_value.tv_usec = 0;
	setitimer(ITIMER_REAL, &timer, nullptr);
	timedOut = s_realTimedOut != 0;
	if (ended)
		*ended = jumped == 2;
	_mm_setcsr(0x9FC0);
	return !faulted;
}

// one run of a chain in a mode: once to find the pages it writes (warming the caches), then again
// from the same state, timed
static void runChain(ChainRun& run, int mode, PPCInterpreter_t& result, const RealAotTest& t)
{
	static bool s_alarm = false;
	if (!s_alarm)
	{
		struct sigaction sa{};
		sa.sa_sigaction = onRealTimeout;
		sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
		sigaction(SIGALRM, &sa, nullptr);
		s_alarm = true;
	}
	PPCInterpreter_t& cpu = runCpu();
	bool timedOut = false;
	restoreDirtyPages(t, true);
	s_pageTracking = 1;
	runChainOnce(run, cpu, timedOut);
	// JT_AOT_WARMUPS: more untimed runs first (one left the caches and branch predictors less warm
	// than for a mode run right after another mode's run of the same chain)
	static const int warmups = getenv("JT_AOT_WARMUPS") ? atoi(getenv("JT_AOT_WARMUPS")) : 3;
	for (int w = 1; w < warmups && !timedOut; w++)
	{
		restoreDirtyPages(t, false);
		runChainOnce(run, cpu, timedOut);
	}
	restoreDirtyPages(t, false);
	const uint64 t0 = __rdtsc();
	run.faulted[mode] = !runChainOnce(run, cpu, timedOut, &run.ended[mode]);
	run.ticks[mode] = __rdtsc() - t0;
	s_pageTracking = 0;
	run.timedOut[mode] = timedOut;
	result = cpu;
	run.hash[mode] = hashDirtyPages();
}

static int runAotChains(RealAotTest& t, const char* path, uint32 codeStart, uint32 codeEnd)
{
	CremaCodeTestForget(codeStart, codeEnd - codeStart);
	uint32 counts[4];
	std::string why;
	const auto loadStart = std::chrono::steady_clock::now();
	if (!CremaAotTestLoad(path, counts, why))
	{
		printf("AOT chains: %s can't be used: %s\n", path, why.c_str());
		return 1;
	}
	printf("AOT chains: %u functions made active in %.0f ms, %u compiled meanwhile, %u changed, %u invalid\n", counts[0],
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count(), counts[1], counts[2], counts[3]);
	const CremaAotManifest* manifest = CremaAotTestManifest();
	if (!manifest)
		return 1;
	s_aotAbi = getenv("JT_AOT_ABI") && atoi(getenv("JT_AOT_ABI")) != 0;
	g_cremaAotTestEnd = onAotTestEnd;
	const int count = getenv("JT_AOT_CHAINS") ? atoi(getenv("JT_AOT_CHAINS")) : 20000;
	std::vector<ChainRun> runs(count);
	std::vector<PPCInterpreter_t> results(count);
	for (int i = 0; i < count; i++)
	{
		const CremaAotRecord& rec = manifest->records[rnd(manifest->count)];
		runs[i].start = rec.entry;
		initRealState(runs[i].initial, t);
	}
	uint64 differ = 0, faultDiffer = 0, compared = 0, bothFaulted = 0, timedOut = 0, ticks[3] = {};
	PPCInterpreter_t other;
	// the scratch area write-protected: each run restores only the pages the one before wrote
	// (runChain: a run to find them, then the timed run with code and data in the caches)
	memcpy(memory_base + kRealData, t.pristine.data(), kRealSize);
	mprotect(memory_base + kRealData, kRealSize, PROT_READ);
	memset(s_pageDirty, 0, sizeof(s_pageDirty));
	s_dirtyCount = 0;
	for (int i = 0; i < count; i++)
	{
		ChainRun& run = runs[i];
		CremaAotTestDirectCalls(true);
		runChain(run, 0, results[i], t);
		if (s_aotAbi)
		{
			// (EABI calls need the direct ones: through the bridge every call would end the run)
			if (run.timedOut[0])
				timedOut++;
			if (run.faulted[0])
				bothFaulted++;
			else
			{
				compared++;
				ticks[0] += run.ticks[0];
			}
			continue;
		}
		CremaAotTestDirectCalls(false);
		runChain(run, 1, other, t);
		if (run.timedOut[0] || run.timedOut[1])
			timedOut++;
		if (run.faulted[0] || run.faulted[1])
		{
			if (run.faulted[0] != run.faulted[1])
			{
				faultDiffer++;
				if (faultDiffer <= 10)
					printf("CHAIN DIFF %d from %08x: faulted %s only (timed out: %d %d)\n", i, run.start, run.faulted[0] ? "with direct calls" : "through the bridge",
						run.timedOut[0], run.timedOut[1]);
			}
			else
				bothFaulted++;
			continue;
		}
		compared++;
		ticks[0] += run.ticks[0];
		ticks[1] += run.ticks[1];
		bool nanOnly;
		std::string what;
		if (!sameRealState(results[i], other, nanOnly, what) || run.hash[0] != run.hash[1])
		{
			differ++;
			if (differ <= 20)
				printf("CHAIN DIFF %d from %08x: direct calls / bridge: %s\n", i, run.start, what.empty() ? "memory" : what.c_str());
		}
	}
	CremaAotTestDirectCalls(true);
	printf("AOT chains: %d runs, %llu compared, %llu faulted on both sides (%llu of them never left, stopped), %llu faulted on one side; direct calls vs "
		"bridge: %llu differ; TSC ticks per run: direct calls %.0f, bridge %.0f\n", count, (unsigned long long)compared, (unsigned long long)bothFaulted,
		(unsigned long long)timedOut, (unsigned long long)faultDiffer, (unsigned long long)differ, compared ? (double)ticks[0] / compared : 0.0,
		compared ? (double)ticks[1] / compared : 0.0);
	if (!s_cachePath)
		return differ || faultDiffer ? 1 : 0;
	// the JIT's code of every function instead (the compiled code cache this run recorded)
	CremaCodeTestWrite(s_cachePath, true);
	CremaCodeTestForget(codeStart, codeEnd - codeStart);
	CremaCodeTestReset();
	uint32 cacheCounts[5];
	CremaCodeTestLoad(s_cachePath, cacheCounts);
	printf("AOT chains: the JIT's code: %u functions loaded from the compiled code cache\n", cacheCounts[0]);
	uint64 jitDiffer = 0, jitFaultDiffer = 0, jitCompared = 0, jitTicks = 0, aotTicks = 0, ended = 0;
	for (int i = 0; i < count; i++)
	{
		ChainRun& run = runs[i];
		runChain(run, 2, other, t);
		if (run.faulted[0] || run.faulted[2])
		{
			if (run.faulted[0] != run.faulted[2] && ++jitFaultDiffer <= 10)
				printf("CHAIN DIFF %d from %08x: faulted %s only (timed out: %d %d)\n", i, run.start, run.faulted[0] ? "translated" : "JIT", run.timedOut[0],
					run.timedOut[2]);
			continue;
		}
		jitCompared++;
		jitTicks += run.ticks[2];
		aotTicks += run.ticks[0];
		if (run.ended[0])
		{
			// (stopped inside translated code: timed, not compared - see s_aotAbi)
			ended++;
			continue;
		}
		bool nanOnly;
		std::string what;
		if (!sameRealState(results[i], other, nanOnly, what) || run.hash[0] != run.hash[2])
		{
			jitDiffer++;
			if (jitDiffer <= 20)
				printf("CHAIN DIFF %d from %08x: translated / JIT: %s\n", i, run.start, what.empty() ? "memory" : what.c_str());
		}
	}
	printf("AOT chains vs JIT: %llu compared (%llu of them stopped inside translated code: timed only), %llu differ, %llu faulted on one side; TSC ticks per run: "
		"JIT %.0f, translated (direct calls) %.0f, ratio %.2f\n", (unsigned long long)jitCompared, (unsigned long long)ended, (unsigned long long)jitDiffer,
		(unsigned long long)jitFaultDiffer, jitCompared ? (double)jitTicks / jitCompared : 0.0, jitCompared ? (double)aotTicks / jitCompared : 0.0,
		jitTicks ? (double)aotTicks / jitTicks : 0.0);
	return differ || faultDiffer || jitDiffer ? 1 : 0;
}

// JT_JIT_CHAIN=n (with JT_RPX and JT_CACHE): n runs of real functions in the JIT's code - every one
// made active by the cache load - from random states (as JT_AOT_REAL), through the jump table with
// their calls and returns until they leave it. One line per run in JT_JIT_CHAIN_OUT (jitchain.txt):
// the start, faulted, timed out, a hash of the CPU state and of the memory written, the TSC ticks.
// The same seed makes the same runs: two builds' files must match apart from the ticks (and runs
// that timed out, cut at a different point).
static void runJitChains(RealAotTest& t, int count, const std::vector<uint32>& entries)
{
	const char* outPath = getenv("JT_JIT_CHAIN_OUT") ? getenv("JT_JIT_CHAIN_OUT") : "jitchain.txt";
	FILE* out = fopen(outPath, "w");
	std::vector<ChainRun> runs(count);
	for (int i = 0; i < count; i++)
	{
		runs[i].start = entries[rnd((uint32)entries.size())];
		initRealState(runs[i].initial, t);
	}
	memcpy(memory_base + kRealData, t.pristine.data(), kRealSize);
	mprotect(memory_base + kRealData, kRealSize, PROT_READ);
	memset(s_pageDirty, 0, sizeof(s_pageDirty));
	s_dirtyCount = 0;
	uint64 faulted = 0, timedOut = 0, ticks = 0, timed = 0;
	PPCInterpreter_t result;
	for (int i = 0; i < count; i++)
	{
		ChainRun& run = runs[i];
		runChain(run, 0, result, t);
		uint64 h = 1469598103934665603ull;
		auto mix = [&](const void* p, size_t n) {
			for (size_t k = 0; k < n; k++)
				h = (h ^ ((const uint8*)p)[k]) * 1099511628211ull;
		};
		mix(result.gpr, sizeof(result.gpr));
		for (int k = 0; k < 32; k++)
		{
			for (double v : {result.fpr[k].fp0, result.fpr[k].fp1})
			{
				if (v != v)
					mix("nan", 3);
				else
					mix(&v, 8);
			}
		}
		mix(result.cr, sizeof(result.cr));
		mix(&result.xer_ca, 3);
		mix(&result.instructionPointer, 4);
		mix(&result.spr.LR, 4);
		mix(&result.spr.CTR, 4);
		mix(&result.remainingCycles, 4);
		faulted += run.faulted[0];
		timedOut += run.timedOut[0];
		if (!run.faulted[0] && !run.timedOut[0])
		{
			ticks += run.ticks[0];
			timed++;
		}
		if (out)
			fprintf(out, "%d %08x %d %d %016llx %016llx %llu\n", i, run.start, run.faulted[0], run.timedOut[0], (unsigned long long)h,
				(unsigned long long)run.hash[0], (unsigned long long)run.ticks[0]);
	}
	if (out)
		fclose(out);
	printf("JIT chains: %d runs, %llu faulted, %llu timed out; TSC ticks per run that left: %.0f (%s)\n", count, (unsigned long long)faulted,
		(unsigned long long)timedOut, timed ? (double)ticks / timed : 0.0, outPath);
}

// JT_RPX=code.bin JT_FUNCS=list: compiles a game's real functions (rpx2bin.py output; the console's
// cache/jit/<title>.txt list of entry points) as PPCRecompiler_recompileAtAddress does - the range
// from the boundary tracker, Crema's return-address entries, the first compile's merge budget - and
// reports each compile's peak allocation (memtrack.cpp) and time, the largest first.
extern std::atomic<long long> g_jtAllocated;
extern std::atomic<long long> g_jtPeak;
void CremaSetRaMergeBudget(uint64 budget);

static int runRpx(const char* binPath, const char* listPath)
{
	FILE* f = fopen(binPath, "rb");
	if (!f)
		return 1;
	// the whole code area readable (zeros where the game's other modules and Cemu's trampolines
	// would be): the boundary tracker follows calls out of the module; JT_MAPPED: only the areas Cemu
	// maps (stubs.cpp)
	extern int g_jtMapped;
	extern uint32 g_jtMappedAreas[4][2];
	g_jtMapped = getenv("JT_MAPPED") ? atoi(getenv("JT_MAPPED")) : 0;
	if (g_jtMapped)
	{
		for (auto& area : g_jtMappedAreas)
			mprotect(memory_base + area[0], area[1] - area[0], PROT_READ | PROT_WRITE);
	}
	else
		mprotect(memory_base, 0x10000000, PROT_READ | PROT_WRITE);
	// Wii: all of MEM1 readable as on the console (zeros past the DOL), so the boundary tracker can
	// follow any branch into it
	mprotect(memory_base + 0x80000000u, 0x01800000, PROT_READ | PROT_WRITE);
	uint32 header[2];
	uint32 codeStart = 0xFFFFFFFF, codeEnd = 0;
	while (fread(header, 4, 2, f) == 2)
	{
		const uint32 addr = header[0], size = header[1];
		const uint32 pageStart = addr & ~0xFFFu, pageEnd = (addr + size + 0xFFF) & ~0xFFFu;
		mprotect(memory_base + pageStart, pageEnd - pageStart, PROT_READ | PROT_WRITE);
		if (fread(memory_base + addr, 1, size, f) != size)
			return 1;
		PPCRecompiler_allocateRange(addr, size);
		codeStart = std::min(codeStart, addr);
		codeEnd = std::max(codeEnd, addr + size);
		fprintf(stderr, "code %08x-%08x\n", addr, addr + size);
	}
	fclose(f);
	struct Result
	{
		uint32 address, length, segments;
		bool ok;
		double ms;
		long long peak;
	};
	std::vector<Result> results;
	results.reserve(50000);
	uint64 x86Bytes = 0; // the compiled functions' x86 code
	const long long startAllocated = g_jtAllocated.load();
	// JT_CACHE=file: every compiled function recorded in the compiled code cache, the file loaded
	// afterwards (all of them made active again); with JT_CACHE_LOAD=1 only loaded (a file written by
	// an earlier run: run both under setarch -R, the cache checks where Crema's code is)
	auto cacheLoad = [&](const char* what, bool forget = true) {
		if (forget)
			CremaCodeTestForget(codeStart, codeEnd - codeStart);
		CremaCodeTestReset();
		uint32 counts[5];
		const auto t0 = std::chrono::steady_clock::now();
		CremaCodeTestLoad(s_cachePath, counts);
		printf("cache %s: %u activated, %u compiled meanwhile, %u changed, %u invalid, %u twice, %.0f ms\n", what, counts[0], counts[1], counts[2],
			counts[3], counts[4], std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	};
	if (s_cachePath && getenv("JT_CACHE_LOAD"))
	{
		cacheLoad("loaded");
		return 0;
	}
	if (s_cachePath)
	{
		CremaCodeTestReset();
		remove(s_cachePath);
	}
	// JT_AOT_OUT=dir: each function also written as C++ (BackendC) - dir/aot_NNNN.cpp, 300 functions
	// each, and dir/manifest.txt: per function its address, entries, the guest ranges it was made from
	// (the loader checks them) - for an ahead-of-time build of the game's code
	const char* aotOut = getenv("JT_AOT_OUT");
	// JT_AOT_DIRECT=0: no direct calls between translated functions (each through the bridge)
	PPCRecompiler_cSetDirectCalls(!getenv("JT_AOT_DIRECT") || atoi(getenv("JT_AOT_DIRECT")) != 0);
	// JT_AOT_ABI=1: calls as the EABI has them (only the volatile registers stored and loaded again)
	PPCRecompiler_cSetAbiCalls(getenv("JT_AOT_ABI") && atoi(getenv("JT_AOT_ABI")) != 0);
	// JT_AOT_KEEP=lo-hi (hex guest addresses: the register save/restore helpers, whose calls load
	// everything again): the non-volatile registers stay in locals across calls, a function entered at
	// its start loads only what it reads (recomp/patch-aot-calls.py)
	if (const char* keep = getenv("JT_AOT_KEEP"))
	{
		char* dash = nullptr;
		const uint32 lo = (uint32)strtoul(keep, &dash, 16);
		const uint32 hi = dash && *dash == '-' ? (uint32)strtoul(dash + 1, nullptr, 16) : 0;
		PPCRecompiler_cSetKeepCalls(true, lo, hi);
		// (JT_AOT_KEEP_PARTS: 1 the kept registers only, 2 the start entries' loads only; default both)
		if (getenv("JT_AOT_KEEP_PARTS"))
			PPCRecompiler_cSetKeepParts(atoi(getenv("JT_AOT_KEEP_PARTS")));
		fprintf(stderr, "aot: registers kept across calls (helpers %08x-%08x load everything)\n", lo, hi);
	}
	// JT_AOT_EXPAND=lo-hi (hex guest addresses: CodeWarrior's register save/restore helpers): calls to
	// them expanded in place (recomp/patch-aot-expand.py on PPCRecompilerImlGen.cpp)
	if (const char* expand = getenv("JT_AOT_EXPAND"))
	{
		char* dash = nullptr;
		const uint32 lo = (uint32)strtoul(expand, &dash, 16);
		const uint32 hi = dash && *dash == '-' ? (uint32)strtoul(dash + 1, nullptr, 16) : 0;
		void PPCRecompiler_setExpandHelpers(uint32 lo, uint32 hi);
		PPCRecompiler_setExpandHelpers(lo, hi);
		fprintf(stderr, "aot: calls to %08x-%08x expanded in place\n", lo, hi);
	}
	FILE* manifest = nullptr;
	std::string shard;
	int shardIndex = 0, shardCount = 0, aotWritten = 0, aotNot = 0, aotKeptFunctions = 0, aotTailBack = 0;
	std::map<std::string, int> aotWhy;
	std::vector<AotManifestFunction> aotRecords;
	std::vector<uint32> shardTargets;
	// (300 functions or 4 MiB of C++ a file: the build runs the files in parallel, and a file of
	// huge functions alone took 21 minutes)
	auto flushShard = [&](bool last) {
		if (!aotOut || (shardCount == 0) || (!last && shardCount < 300 && shard.size() < ((size_t)4 << 20)))
			return;
		char path[512];
		snprintf(path, sizeof(path), "%s/aot_%04d.cpp", aotOut, shardIndex++);
		// (the functions its functions call by name declared before them all)
		std::sort(shardTargets.begin(), shardTargets.end());
		shardTargets.erase(std::unique(shardTargets.begin(), shardTargets.end()), shardTargets.end());
		// a file that is already so is left alone (aotbuild.sh compiles only newer files again)
		const std::string text = PPCRecompiler_cPrelude() + PPCRecompiler_cDeclarations(shardTargets) + shard;
		shardTargets.clear();
		std::string old;
		if (FILE* of = fopen(path, "rb"))
		{
			char buffer[1 << 16];
			for (size_t n; (n = fread(buffer, 1, sizeof(buffer), of)) > 0;)
				old.append(buffer, n);
			fclose(of);
		}
		if (old != text)
			writeFile(path, text);
		shard.clear();
		shardCount = 0;
	};
	if (aotOut)
	{
		system(("mkdir -p '" + std::string(aotOut) + "'").c_str());
		manifest = fopen((std::string(aotOut) + "/manifest.txt").c_str(), "w");
	}
	RealAotTest real;
	const char* chainPath = getenv("JT_AOT_CHAIN");
	if (const char* realPath = getenv("JT_AOT_REAL"))
	{
		std::string why;
		real.manifest = CremaAotTestOpen(realPath, why);
		if (!real.manifest)
		{
			fprintf(stderr, "JT_AOT_REAL: %s can't be used: %s\n", realPath, why.c_str());
			return 1;
		}
		for (uint32 i = 0; i < real.manifest->count; i++)
			real.byEntry[real.manifest->records[i].entry] = i;
		real.runs = getenv("JT_AOT_RUNS") ? atoi(getenv("JT_AOT_RUNS")) : 2;
		fprintf(stderr, "JT_AOT_REAL: %u translated functions, %d runs each\n", real.manifest->count, real.runs);
	}
	const int jitChains = getenv("JT_JIT_CHAIN") ? atoi(getenv("JT_JIT_CHAIN")) : 0;
	if (real.manifest || chainPath || jitChains)
	{
		real.codeStart = codeStart;
		real.codeEnd = codeEnd;
		// everything readable (the game's code, zeros elsewhere), only the scratch area writable
		for (auto& area : g_jtMappedAreas)
			mprotect(memory_base + area[0], area[1] - area[0], PROT_READ);
		// (Wii: the low area and MEM1 were made writable for the boundary tracker; a store through a
		// garbage pointer there would outlive the JIT's run and change what the translated run reads)
		mprotect(memory_base, 0x10000000, PROT_READ);
		mprotect(memory_base + 0x80000000u, 0x01800000, PROT_READ);
		mprotect(memory_base + kRealData, kRealSize, PROT_READ | PROT_WRITE);
		real.pristine.resize(kRealSize);
		real.jitData.resize(kRealSize);
		for (uint32 i = 0; i < kRealSize; i += 4)
		{
			const uint32 v = rnd(8) ? realPointer() : (uint32)rng();
			*(uint32*)(real.pristine.data() + i) = _swapEndianU32(v);
		}
		struct sigaction sa{};
		sa.sa_sigaction = onRealFault;
		sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
	}
	FILE* list = fopen(listPath, "r");
	char line[256];
	const int limit = getenv("JT_LIMIT") ? atoi(getenv("JT_LIMIT")) : 1 << 30;
	// (the console's list appends an address again when its code changed: once each, where first seen)
	std::set<uint32> seen;
	while (list && fgets(line, sizeof(line), list) && (int)results.size() < limit)
	{
		const uint32 address = (uint32)strtoul(line, nullptr, 16);
		if (address < codeStart || address >= codeEnd || (address & 3) || !seen.insert(address).second)
			continue;
		s_currentFunction = address;
		PPCFunctionBoundaryTracker funcBoundaries;
		funcBoundaries.trackStartPoint(address);
		PPCFunctionBoundaryTracker::PPCRange_t range;
		if (!funcBoundaries.getRangeForAddress(address, range))
			continue;
		std::set<uint32> entries{address};
		// Crema's return-address entries (PPCRecompiler_recompileAtAddress)
		for (uint32 v = range.startAddress; v + 4 < range.startAddress + range.length; v += 4)
		{
			const uint32 op = memory_readU32(v);
			const uint32 primary = op >> 26;
			const uint32 xo = (op >> 1) & 0x3FF;
			const bool call = (op & 1) != 0 && (primary == 18 || primary == 16 || (primary == 19 && (xo == 16 || xo == 528)));
			if (!call)
				continue;
			const uint32 next = memory_readU32(v + 4);
			if (next != 0 && (next >> 26) != 0)
				entries.emplace(v + 4);
		}
		std::vector<std::pair<MPTR, uint32>> entryPoints;
		CremaSetRaMergeBudget(200000);
		const long long base = g_jtAllocated.load();
		g_jtPeak.store(base);
		const auto t0 = std::chrono::steady_clock::now();
		PPCRecFunction_t* func = PPCRecompiler_recompileFunction(range, entries, entryPoints, funcBoundaries);
		if (!func && entries.size() > 1)
		{
			entries.clear();
			entries.emplace(address);
			func = PPCRecompiler_recompileFunction(range, entries, entryPoints, funcBoundaries);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		results.push_back({address, range.length, (uint32)entryPoints.size(), func != nullptr, ms, g_jtPeak.load() - base});
		if (func)
		{
			x86Bytes += func->x86Size;
			// JT_PATTERNS=1: short jumps padded with a 4-byte NOP, compares with an imm32 that fits a byte
			if (getenv("JT_PATTERNS"))
			{
				const uint8* c = (const uint8*)func->x86Code;
				for (size_t k = 0; k + 6 <= func->x86Size; k++)
				{
					if (c[k] >= 0x70 && c[k] <= 0x7F && c[k + 2] == 0x0F && c[k + 3] == 0x1F && c[k + 4] == 0x40 && c[k + 5] == 0x00)
						s_jtPaddedJumps++;
					// cmp r32, imm32 (81 /7, optionally after a REX prefix) with a value that fits imm8
					const size_t r = (c[k] >= 0x40 && c[k] <= 0x4F) ? 1 : 0;
					if (k + r + 6 <= func->x86Size && c[k + r] == 0x81 && (c[k + r + 1] & 0xF8) == 0xF8)
					{
						const sint32 v = (sint32)(c[k + r + 2] | c[k + r + 3] << 8 | c[k + r + 4] << 16 | (uint32)c[k + r + 5] << 24);
						if (v >= -128 && v <= 127)
							s_jtWideCompares++;
					}
				}
			}
		}
		// JT_CODE=file with a one-function list: its x86 code (objdump -D -b binary -mi386:x86-64 file)
		if (func && getenv("JT_CODE"))
			writeFile(getenv("JT_CODE"), std::string((const char*)func->x86Code, func->x86Size));
		if (getenv("JT_VERBOSE"))
			fprintf(stderr, "%08x len %u: %s %.1f ms, peak %.1f MiB\n", address, range.length, func ? "ok" : "failed", ms,
				(g_jtPeak.load() - base) / 1048576.0);
		if (func && s_cachePath)
		{
			CremaCodeTestRecord(address, range, funcBoundaries.GetRanges(), func, entryPoints);
			CremaCodeTestWrite(s_cachePath, false);
		}
		if (func && real.manifest)
			runRealAot(real, address, func, entryPoints);
		if (func && aotOut)
		{
			// the same compile (the entries the JIT's one ended with) as C++
			AotFunctionOut out;
			char name[32];
			snprintf(name, sizeof(name), "aot_%08x", address);
			s_aotOut = &out;
			s_aotName = name;
			s_aotOk = false;
			s_aotWhy = "no intermediate code";
			g_jtAotHook = aotHook;
			std::vector<std::pair<MPTR, uint32>> unused;
			PPCFunctionBoundaryTracker tracker2;
			tracker2.trackStartPoint(address);
			PPCFunctionBoundaryTracker::PPCRange_t range2;
			tracker2.getRangeForAddress(address, range2);
			PPCRecompiler_recompileFunction(range2, entries, unused, tracker2);
			g_jtAotHook = nullptr;
			if (s_aotOk)
			{
				shard += out.code;
				shardTargets.insert(shardTargets.end(), out.directTargets.begin(), out.directTargets.end());
				shardCount++;
				aotWritten++;
				aotTailBack += out.tailCallsBack;
				if (out.keptInputs)
				{
					// (EABI calls: a function reading its caller's non-volatile registers; listed)
					if (aotKeptFunctions++ < 40)
						printf("AOT EABI: %s reads %u non-volatile registers as its caller left them\n", name, out.keptInputs);
				}
				fprintf(manifest, "%08x %zu %u", address, out.entries.size(), func->x86Size);
				for (uint32 e : out.entries)
					fprintf(manifest, " %08x", e);
				const auto ranges = tracker2.GetRanges();
				fprintf(manifest, " | %zu", ranges.size());
				for (auto& r : ranges)
					fprintf(manifest, " %08x:%x", r.startAddress, r.length);
				fprintf(manifest, "\n");
				AotManifestFunction rec;
				rec.name = name;
				rec.entry = address;
				rec.rangeStart = range2.startAddress;
				rec.rangeLength = range2.length;
				for (auto& r : ranges)
					rec.hashRanges.emplace_back(r.startAddress, r.length);
				rec.entries = out.entries;
				rec.guestHash = CremaCodeTestGuestHash(ranges);
				aotRecords.push_back(std::move(rec));
				flushShard(false);
			}
			else
			{
				aotNot++;
				aotWhy[s_aotWhy]++;
			}
		}
		delete func;
	}
	if (aotOut)
	{
		flushShard(true);
		fclose(manifest);
		// the manifest the loader reads (PPCRecompiler.cpp CremaAot*): built with the other files
		writeFile(std::string(aotOut) + "/aot_manifest.cpp", PPCRecompiler_cManifest(aotRecords));
		// files an earlier run with more of them left behind (aotbuild.sh builds every aot_*.cpp there:
		// their functions would be defined twice)
		for (int stale = shardIndex;; stale++)
		{
			char path[512];
			snprintf(path, sizeof(path), "%s/aot_%04d.cpp", aotOut, stale);
			if (remove(path) != 0)
				break;
		}
		printf("AOT: %d functions written as C++ in %d files, %d not:\n", aotWritten, shardIndex, aotNot);
		for (auto& [why, n] : aotWhy)
			printf("  %s x%d\n", why.c_str(), n);
		printf("AOT EABI: %d functions read non-volatile registers as their callers left them, %d calls come back where their caller has no entry\n",
			aotKeptFunctions, aotTailBack);
	}
	if (real.manifest)
		printf("AOT real: %llu functions run (%llu compiled by the JIT not in the file, %llu with other entries), %llu runs compared, "
			"%llu faulted on both sides, %llu left to a garbage address (compared), %llu faulted on one side only; %llu differ (%llu more in NaNs only); "
			"JIT %.0f, translated %.0f TSC ticks per run\n",
			(unsigned long long)real.functions, (unsigned long long)real.missing, (unsigned long long)real.entryMismatch, (unsigned long long)real.compared,
			(unsigned long long)real.bothFaulted, (unsigned long long)real.tableFaults, (unsigned long long)real.faultMismatch, (unsigned long long)real.differ,
			(unsigned long long)real.nanOnly,
			real.compared ? (double)real.jitTicks / real.compared : 0.0, real.compared ? (double)real.aotTicks / real.compared : 0.0);
	if (chainPath)
		return runAotChains(real, chainPath, codeStart, codeEnd);
	if (s_cachePath)
	{
		CremaCodeTestWrite(s_cachePath, true);
		cacheLoad("written and loaded");
		cacheLoad("loaded again over the active ones (all 'compiled meanwhile' expected)", false);
		if (jitChains)
		{
			std::vector<uint32> entries;
			for (auto& r : results)
				if (r.ok)
					entries.push_back(r.address);
			runJitChains(real, jitChains, entries);
		}
	}
	int failed = 0;
	double totalMs = 0;
	for (auto& r : results)
	{
		failed += !r.ok;
		totalMs += r.ms;
	}
	printf("compiled %zu functions, %d failed, %.0f ms in all, %.1f MiB still allocated afterwards, %llu bytes of x86 code\n", results.size(), failed, totalMs,
		(g_jtAllocated.load() - startAllocated) / 1048576.0, (unsigned long long)x86Bytes);
	// JT_BYTES=1: the x86 code by intermediate instruction (type, operation or register kind), largest first
	if (getenv("JT_BYTES"))
	{
		static const char* typeNames[] = {"NONE", "NO_OP", "R_R", "R_R_R", "R_R_R_CARRY", "R_R_S32", "R_R_S32_CARRY", "LOAD",
			"LOAD_INDEXED", "STORE", "STORE_INDEXED", "R_NAME", "NAME_R", "R_S32", "MACRO", "CJUMP_CYCLE_CHECK", "COMPARE",
			"COMPARE_S32", "JUMP", "CONDITIONAL_JUMP", "ATOMIC_CMP_STORE", "CALL_IMM", "FPR_LOAD", "FPR_LOAD_INDEXED", "FPR_STORE",
			"FPR_STORE_INDEXED", "FPR_R_R", "FPR_R_R_R", "FPR_R_R_R_R", "FPR_R", "FPR_COMPARE", "X86_EFLAGS_JCC"};
		struct Row
		{
			uint32 type, key;
			uint64 bytes, count;
		};
		std::vector<Row> rows;
		uint64 sum = 0;
		for (uint32 t = 0; t < 64; t++)
			for (uint32 k = 0; k < 256; k++)
				if (g_jtImlBytes[t][k])
				{
					rows.push_back({t, k, g_jtImlBytes[t][k], g_jtImlCount[t][k]});
					sum += g_jtImlBytes[t][k];
				}
		std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.bytes > y.bytes; });
		printf("x86 code by intermediate instruction: %llu of the %llu bytes (the rest: cold code)\n", (unsigned long long)sum,
			(unsigned long long)x86Bytes);
		static const char* sourceNames[] = {"live-in f1-f8", "live-in f0/f9-f13", "live-in f14-f31", "live-in other",
			"lfs/psq_l", "lfd", "double arithmetic", "fmr", "other FPR_R_R", "FPR_R", "other"};
		printf("single stores left exact, by where the value comes from:");
		for (int k = 0; k < 11; k++)
			printf(" %s %llu", sourceNames[k], (unsigned long long)g_jtSingleStoreSource[k]);
		printf("\n  FPR_R operations among them:");
		for (int k = 0; k < 64; k++)
			if (g_jtSingleStoreFprR[k])
				printf(" %d: %llu", k, (unsigned long long)g_jtSingleStoreFprR[k]);
		printf("\n");
		for (size_t r = 0; r < rows.size() && r < 50; r++)
			printf("  %-18s %3u %11llu bytes %5.1f%% %10llu x %5.1f bytes\n", rows[r].type < 32 ? typeNames[rows[r].type] : "?", rows[r].key,
				(unsigned long long)rows[r].bytes, 100.0 * rows[r].bytes / x86Bytes, (unsigned long long)rows[r].count,
				(double)rows[r].bytes / rows[r].count);
	}
	if (getenv("JT_PATTERNS"))
		printf("patterns: %llu short jumps padded with a 4-byte NOP, %llu compares with a byte-sized imm32\n", (unsigned long long)s_jtPaddedJumps,
			(unsigned long long)s_jtWideCompares);
	printf("register allocation: at most %u rounds (one spill each) in a function, at %08x\n", g_jtRaMaxRounds, g_jtRaMaxRoundsAt);
	std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) { return a.peak > b.peak; });
	printf("largest allocation peaks:\n");
	for (size_t i = 0; i < results.size() && i < 25; i++)
		printf("  %08x %6u bytes of code, %s, %8.1f ms, peak %8.1f MiB\n", results[i].address, results[i].length,
			results[i].ok ? "ok    " : "FAILED", results[i].ms, results[i].peak / 1048576.0);
	std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) { return a.ms > b.ms; });
	printf("slowest:\n");
	for (size_t i = 0; i < results.size() && i < 15; i++)
		printf("  %08x %6u bytes of code, %s, %8.1f ms, peak %8.1f MiB\n", results[i].address, results[i].length,
			results[i].ok ? "ok    " : "FAILED", results[i].ms, results[i].peak / 1048576.0);
	if (getenv("JT_RESULTS"))
	{
		FILE* out = fopen(getenv("JT_RESULTS"), "w");
		for (auto& r : results)
			fprintf(out, "%08x %u %d %.2f %lld\n", r.address, r.length, r.ok ? 1 : 0, r.ms, r.peak);
		fclose(out);
	}
	return 0;
}


// JT_AOT=1: ahead-of-time compiles (Cafe/HW/Espresso/Recompiler/BackendC). Each program's code is
// also written as C++ from the same intermediate code; all of them are built by clang -O2 into one
// shared library, and each program runs again from that, compared with the JIT's run (they must be
// identical: same IML, two backends) and the interpreter's. JT_AOT_DIR: where the C++ and the
// library go (/tmp/jtaot). JT_BENCH=n also times n runs of each.
typedef uint32 (*AotFn)(uint8*, uint8*, uint32);

struct AotImportsHost
{
	double (ATTR_MS_ABI* fres)(double);
	double (ATTR_MS_ABI* frsqrte)(double);
	uint32 (ATTR_MS_ABI* tbl)();
	uint32 (ATTR_MS_ABI* tbu)();
	const void* table; // the jump table; no stubs here: the functions never call each other directly
	const uint8* stubs;
	uintptr_t stubBytes;
	uint32 (*hle)(uint8* h, uint32 target); // no OS here: never an OS function's stub
};

static uint32 noHle(uint8*, uint32)
{
	return 0;
}
ATTR_MS_ABI double frsqrte_espresso(double input);
ATTR_MS_ABI double fres_espresso(double input);
ATTR_MS_ABI uint32 PPCRecompiler_GetTBL();
ATTR_MS_ABI uint32 PPCRecompiler_GetTBU();

// one run of a program's C++ function from start, as runRecompiled does for its x86 code: control
// that goes anywhere but the end (kReturn) stops there (the JIT's code leaves through its jump table)
static bool runAot(PPCInterpreter_t* cpu, AotFn fn, const std::vector<uint32>& entries, uint32 start)
{
	if (!fn || std::find(entries.begin(), entries.end(), start) == entries.end())
		return false;
	PPCInterpreter_setCurrentInstance(cpu);
	cpu->instructionPointer = start;
	// (the host stack a function's depth check measures from, as the JIT's entry sets it)
	cpu->rspTemp = __builtin_frame_address(0);
	const uint32 next = fn((uint8*)cpu, memory_base, start);
	cpu->instructionPointer = next & ~1u;
	return true;
}

struct AotCase
{
	Program prog;
	State initial;
	std::vector<std::pair<uint32, EntryKind>> extra;
	std::set<uint32> entryAddresses;
	PPCRecFunction_t* jit = nullptr;
	std::vector<std::pair<MPTR, uint32>> entryPoints;
	bool aot = false;
	std::vector<uint32> aotEntries;
};

// a difference only in NaNs: both sides NaN where the bits differ (sign and payload of a NaN result
// depend on the operand order, which the compiler may swap in a commutative operation)
static bool nanOnlyDifference(const PPCInterpreter_t& x, const PPCInterpreter_t& y, const uint8* dx, const uint8* dy)
{
	if (memcmp(x.gpr, y.gpr, sizeof(x.gpr)) != 0 || memcmp(x.cr, y.cr, sizeof(x.cr)) != 0 || x.xer_ca != y.xer_ca || x.xer_so != y.xer_so ||
		x.instructionPointer != y.instructionPointer || x.spr.CTR != y.spr.CTR)
		return false;
	for (int i = 0; i < 32; i++)
		for (int lane = 0; lane < 2; lane++)
		{
			const double a = lane ? x.fpr[i].fp1 : x.fpr[i].fp0, b = lane ? y.fpr[i].fp1 : y.fpr[i].fp0;
			if (memcmp(&a, &b, 8) != 0 && !(a != a && b != b))
				return false;
		}
	auto isNan = [](uint32 w) { return ((w >> 23) & 0xFF) == 0xFF && (w & 0x7FFFFF) != 0; };
	for (uint32 i = 0; i < kDataSize; i += 4)
	{
		const uint32 a = _swapEndianU32(*(const uint32*)(dx + i)), b = _swapEndianU32(*(const uint32*)(dy + i));
		if (a != b && !(isNan(a) && isNan(b)))
			return false;
	}
	return true;
}

static void placeCode(const Program& prog)
{
	for (size_t i = 0; i < prog.code.size(); i++)
		*(uint32*)(memory_base + kCode + i * 4) = _swapEndianU32(prog.code[i]);
}

// JT_AOT_LOADER: a run through the jump table, as Crema enters recompiled code (for a translated
// function: its entry's stub, the bridge, the function, the bridge again, out through the table)
static bool runThroughTable(PPCInterpreter_t* cpu, uint32 start)
{
	PPCInterpreter_setCurrentInstance(cpu);
	auto entry = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[start / 4];
	if (entry == PPCRecompiler_leaveRecompilerCode_unvisited || entry == PPCRecompiler_leaveRecompilerCode_visited)
		return false;
	cpu->instructionPointer = start;
	auto enter = (void (*)(uint64, uint64, uint64, uint64))PPCRecompiler_enterRecompilerCode;
	enter(0, 0, (uint64)cpu, (uint64)entry);
	return true;
}

// a translated code file built by aotbuild.sh (as a game's) from C++ files in dir: false if it failed
static bool buildAotFile(const std::string& dir, const std::string& file)
{
	const char* script = getenv("JT_AOT_BUILD");
	const std::string build = std::string("bash '") + (script ? script : (std::string(getenv("HOME")) + "/cemups4/tests/jittest/aotbuild.sh").c_str()) +
		"' '" + dir + "' '" + file + "' > '" + dir + "/build.log' 2>&1";
	if (system(build.c_str()) == 0)
		return true;
	fprintf(stderr, "translated code file build failed (%s/build.log)\n", dir.c_str());
	return false;
}

static void writeFile(const std::string& path, const std::string& text)
{
	if (FILE* f = fopen(path.c_str(), "wb"))
	{
		fwrite(text.data(), 1, text.size(), f);
		fclose(f);
	}
}

static int runAotTest(int count, int length, bool verbose)
{
	const char* dirEnv = getenv("JT_AOT_DIR");
	const std::string dir = dirEnv ? dirEnv : "/tmp/jtaot";
	std::vector<AotCase> cases(count);
	std::string source = PPCRecompiler_cPrelude();
	std::map<std::string, int> whyNot;
	int notAot = 0;
	// JT_AOT_LOADER=1: the programs also built as a translated code file (aotbuild.sh, as a game's),
	// each loaded by Crema's loader (PPCRecompiler.cpp CremaAot*: made active in the jump table where
	// its code matches) and entered as recompiled code is (stub, bridge, function, out through the
	// table); compared with the JIT's run, cycles too
	const bool loaderTest = getenv("JT_AOT_LOADER") != nullptr;
	std::string loaderSource = PPCRecompiler_cPrelude();
	std::vector<AotManifestFunction> loaderRecords;
	std::vector<uint32> sourceTargets;
	// the loader hashes 16 bytes after each range: what follows a program is cleared (else it is
	// whatever longer program was there before, other at the load than when the record was made)
	auto place = [&](const Program& prog) {
		placeCode(prog);
		if (loaderTest)
			memset(memory_base + kCode + prog.code.size() * 4, 0, 64);
	};
	for (int t = 0; t < count; t++)
	{
		AotCase& c = cases[t];
		c.prog = makeProgram(length);
		initState(c.initial);
		place(c.prog);
		for (int e = 0; e < s_entries && !c.prog.entryCandidates.empty(); e++)
		{
			auto pick = c.prog.entryCandidates[rnd((uint32)c.prog.entryCandidates.size())];
			if (pick.second == kEntryAny && rnd(4) != 0)
				pick = c.prog.entryCandidates[rnd((uint32)c.prog.entryCandidates.size())];
			if (pick.first == 0 || std::any_of(c.extra.begin(), c.extra.end(), [&](const auto& x) { return x.first == pick.first; }))
				continue;
			c.extra.push_back(pick);
		}
		c.entryAddresses.insert(kCode);
		for (auto& [word, kind] : c.extra)
			c.entryAddresses.insert(kCode + word * 4);
		PPCFunctionBoundaryTracker::PPCRange_t range;
		std::vector<PPCFunctionBoundaryTracker::PPCRange_t> tracked;
		c.jit = compileProgram(c.entryAddresses, c.entryPoints, range, tracked);
		// the same compile, written as C++
		AotFunctionOut out;
		s_aotOut = &out;
		s_aotName = "aot_t" + std::to_string(t);
		s_aotOk = false;
		s_aotWhy = "no intermediate code";
		g_jtAotHook = aotHook;
		std::vector<std::pair<MPTR, uint32>> unused;
		compileProgram(c.entryAddresses, unused, range, tracked);
		g_jtAotHook = nullptr;
		if (s_aotOk)
		{
			c.aot = true;
			c.aotEntries = out.entries;
			source += out.code;
			sourceTargets.insert(sourceTargets.end(), out.directTargets.begin(), out.directTargets.end());
			if (loaderTest)
			{
				loaderSource += out.code;
				AotManifestFunction rec;
				rec.name = s_aotName;
				rec.entry = kCode;
				rec.rangeStart = range.startAddress;
				rec.rangeLength = range.length;
				for (auto& r : tracked)
					rec.hashRanges.emplace_back(r.startAddress, r.length);
				rec.entries = out.entries;
				rec.guestHash = CremaCodeTestGuestHash(tracked);
				loaderRecords.push_back(std::move(rec));
			}
		}
		else
		{
			notAot++;
			whyNot[s_aotWhy]++;
		}
	}
	// the fixed addresses the programs jump to by name, declared before them (weak: no such functions)
	std::sort(sourceTargets.begin(), sourceTargets.end());
	sourceTargets.erase(std::unique(sourceTargets.begin(), sourceTargets.end()), sourceTargets.end());
	const std::string preludeText = PPCRecompiler_cPrelude(), declarations = PPCRecompiler_cDeclarations(sourceTargets);
	source.insert(preludeText.size(), declarations);
	loaderSource.insert(preludeText.size(), declarations);
	const std::string loaderDir = dir + "/loader", loaderFile = loaderDir + "/test.aot";
	if (loaderTest)
	{
		system(("rm -rf '" + loaderDir + "' && mkdir -p '" + loaderDir + "'").c_str());
		writeFile(loaderDir + "/aot_0000.cpp", loaderSource);
		writeFile(loaderDir + "/aot_manifest.cpp", PPCRecompiler_cManifest(loaderRecords));
		if (!buildAotFile(loaderDir, loaderFile))
			return 1;
	}
	int loaderLoads = 0, loaderBadLoads = 0, loaderRuns = 0, loaderDiffer = 0, loaderCycles = 0, loaderChangedOk = 0, loaderChangedTried = 0;
	source += "extern \"C\" __attribute__((visibility(\"default\"))) AotImports aot_imports = {};\n";
	source += "typedef uint32 (*AotFnT)(uint8*, uint8*, uint32);\nextern \"C\" __attribute__((visibility(\"default\"))) AotFnT aot_table[] = {\n";
	for (int t = 0; t < count; t++)
		source += cases[t].aot ? "aot_t" + std::to_string(t) + ",\n" : std::string("nullptr,\n");
	source += "};\n";
	system(("mkdir -p '" + dir + "'").c_str());
	const std::string cpp = dir + "/aot-test.cpp", so = dir + "/aot-test.so";
	if (FILE* f = fopen(cpp.c_str(), "wb"))
	{
		fwrite(source.data(), 1, source.size(), f);
		fclose(f);
	}
	// (the functions called here directly, not made active by a loader: their flags start set)
	const std::string cmd = "clang++-21 -std=c++17 -O2 -fPIC -shared -march=btver2 -mno-sse4a -ffp-contract=off -fdenormal-fp-math=preserve-sign -fno-strict-aliasing "
		"-DAOT_FLAG_INIT=1 -Wno-everything -o '" + so + "' '" + cpp + "'";
	const uint64 buildStart = __rdtsc();
	if (system(cmd.c_str()) != 0)
	{
		fprintf(stderr, "AOT build failed: %s\n", cmd.c_str());
		return 1;
	}
	const double buildSeconds = (double)(__rdtsc() - buildStart) / 3.0e9;
	void* lib = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!lib)
	{
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	auto* imports = (AotImportsHost*)dlsym(lib, "aot_imports");
	auto* table = (AotFn*)dlsym(lib, "aot_table");
	if (!imports || !table)
	{
		fprintf(stderr, "AOT library without its table\n");
		return 1;
	}
	imports->fres = fres_espresso;
	imports->frsqrte = frsqrte_espresso;
	imports->tbl = PPCRecompiler_GetTBL;
	imports->tbu = PPCRecompiler_GetTBU;
	imports->table = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
	imports->stubs = nullptr;
	imports->stubBytes = 0;
	imports->hle = noHle;

	int differJit = 0, differItp = 0, compared = 0, nanOnly = 0;
	uint64 jitTicks = 0, aotTicks = 0, benchRuns = 0;
	for (int t = 0; t < count; t++)
	{
		AotCase& c = cases[t];
		place(c.prog);
		bool loaded = false;
		if (loaderTest && c.aot)
		{
			// every program is at kCode: only this one's record matches the code there
			uint32 counts[4];
			std::string why;
			if (t % 8 == 0)
			{
				// one instruction changed: nothing made active
				uint32* word = (uint32*)(memory_base + kCode) + (uint32)t % std::max<size_t>(c.prog.code.size(), 1);
				const uint32 original = *word;
				*word ^= _swapEndianU32(0x00200000);
				CremaCodeTestForget(kCode, 0x10000);
				const bool ok = CremaAotTestLoad(loaderFile, counts, why);
				*word = original;
				loaderChangedTried++;
				if (ok && counts[0] == 0)
					loaderChangedOk++;
				else
					printf("%d translated file with a changed instruction: %s, %u made active\n", t, ok ? "loaded" : why.c_str(), ok ? counts[0] : 0);
			}
			CremaCodeTestForget(kCode, 0x10000);
			loaded = CremaAotTestLoad(loaderFile, counts, why) && counts[0] == 1;
			loaderLoads++;
			if (!loaded)
			{
				loaderBadLoads++;
				printf("%d translated file: %s, %u made active, %u compiled meanwhile, %u changed, %u invalid\n", t, why.empty() ? "loaded" : why.c_str(), counts[0],
					counts[1], counts[2], counts[3]);
			}
		}
		for (int e = -1; e < (int)c.extra.size(); e++)
		{
			const uint32 start = e < 0 ? kCode : kCode + c.extra[e].first * 4;
			State itp = c.initial;
			memcpy(memory_base + kData, c.initial.data.data(), kDataSize);
			_mm_setcsr(0x9FC0);
			runInterpreter(&itp.cpu, start);
			std::vector<uint8> itpData(memory_base + kData, memory_base + kData + kDataSize);
			const uint64 hi = hashState(itp.cpu, itpData.data());

			State jit = c.initial;
			memcpy(memory_base + kData, c.initial.data.data(), kDataSize);
			_mm_setcsr(0x9FC0);
			const bool jitOk = c.jit && runRecompiled(&jit.cpu, c.jit, c.entryPoints, start);
			std::vector<uint8> jitData(memory_base + kData, memory_base + kData + kDataSize);
			const uint64 hj = hashState(jit.cpu, jitData.data());

			State aot = c.initial;
			memcpy(memory_base + kData, c.initial.data.data(), kDataSize);
			_mm_setcsr(0x9FC0);
			const bool aotOk = c.aot && runAot(&aot.cpu, table[t], c.aotEntries, start);
			std::vector<uint8> aotData(memory_base + kData, memory_base + kData + kDataSize);
			const uint64 ha = hashState(aot.cpu, aotData.data());
			if (loaded && jitOk)
			{
				State ld = c.initial;
				memcpy(memory_base + kData, c.initial.data.data(), kDataSize);
				_mm_setcsr(0x9FC0);
				if (runThroughTable(&ld.cpu, start))
				{
					std::vector<uint8> ldData(memory_base + kData, memory_base + kData + kDataSize);
					loaderRuns++;
					const uint64 hl = hashState(ld.cpu, ldData.data());
					const bool same = hl == hj || nanOnlyDifference(ld.cpu, jit.cpu, ldData.data(), jitData.data());
					if (!same)
					{
						loaderDiffer++;
						printf("%d from %08x: loaded %016llx jit %016llx LOADED/JIT DIFF\n", t, start, (unsigned long long)hl, (unsigned long long)hj);
						if (verbose)
							describe("loaded/jit", ld.cpu, jit.cpu, ldData.data(), jitData.data());
					}
					if (ld.cpu.remainingCycles != jit.cpu.remainingCycles)
					{
						loaderCycles++;
						printf("%d from %08x: cycles left %d (loaded) %d (jit)\n", t, start, ld.cpu.remainingCycles, jit.cpu.remainingCycles);
					}
				}
				else
				{
					loaderDiffer++;
					printf("%d from %08x: no translated entry in the jump table\n", t, start);
				}
			}
			if (!jitOk || !aotOk)
				continue;
			compared++;
			bool sameJit = ha == hj;
			const bool sameItp = ha == hi;
			if (!sameJit && nanOnlyDifference(aot.cpu, jit.cpu, aotData.data(), jitData.data()))
			{
				nanOnly++;
				sameJit = true;
			}
			if (!sameJit)
				differJit++;
			if (!sameItp)
				differItp++;
			if (!sameJit || (verbose && !sameItp))
			{
				printf("%d from %08x: aot %016llx jit %016llx itp %016llx%s\n", t, start, (unsigned long long)ha, (unsigned long long)hj, (unsigned long long)hi,
					sameJit ? "" : " AOT/JIT DIFF");
				if (verbose)
				{
					describe("aot/jit", aot.cpu, jit.cpu, aotData.data(), jitData.data());
					disassemble(c.prog);
				}
			}
			if (s_bench && start == kCode)
			{
				PPCInterpreter_t* copy = (PPCInterpreter_t*)aligned_alloc(64, (sizeof(PPCInterpreter_t) + 63) & ~63);
				auto enter = (void (*)(uint64, uint64, uint64, uint64))PPCRecompiler_enterRecompilerCode;
				uint8* entry = (uint8*)c.jit->x86Code;
				for (auto& [ppc, x86] : c.entryPoints)
					if (ppc == kCode)
						entry = (uint8*)c.jit->x86Code + x86;
				uint64 t0 = __rdtsc();
				for (int i = 0; i < s_bench; i++)
				{
					memcpy(copy, &c.initial.cpu, sizeof(PPCInterpreter_t));
					enter(0, 0, (uint64)copy, (uint64)entry);
				}
				uint64 t1 = __rdtsc();
				for (int i = 0; i < s_bench; i++)
				{
					memcpy(copy, &c.initial.cpu, sizeof(PPCInterpreter_t));
					copy->rspTemp = __builtin_frame_address(0);
					table[t]((uint8*)copy, memory_base, kCode);
				}
				uint64 t2 = __rdtsc();
				jitTicks += t1 - t0;
				aotTicks += t2 - t1;
				benchRuns += s_bench;
				free(copy);
			}
		}
	}
	fprintf(stderr, "AOT: %d programs, %d not written as C++, %d runs compared: %d differ from the JIT (%d more in NaNs only), %d from the interpreter; C++ %zu KiB, built in %.1f s\n",
		count, notAot, compared, differJit, nanOnly, differItp, source.size() / 1024, buildSeconds);
	for (auto& [why, n] : whyNot)
		fprintf(stderr, "  not C++: %s x%d\n", why.c_str(), n);
	if (benchRuns)
		fprintf(stderr, "AOT bench: JIT %.1f, AOT %.1f TSC ticks per run (incl. state copy), AOT/JIT %.2f\n", (double)jitTicks / benchRuns, (double)aotTicks / benchRuns,
			(double)aotTicks / std::max<uint64>(jitTicks, 1));
	if (loaderTest)
		fprintf(stderr, "AOT loader: %d loads (%d wrong), %d of %d changed-code loads made nothing active; %d runs through the jump table: %d differ from "
			"the JIT, %d in cycles (Cycle Counter: %s)\n", loaderLoads, loaderBadLoads, loaderChangedOk, loaderChangedTried, loaderRuns, loaderDiffer, loaderCycles,
			g_crema_cycles_in_register ? "Register" : "Memory");
	return differJit || loaderDiffer || loaderBadLoads || loaderCycles || loaderChangedOk != loaderChangedTried ? 1 : 0;
}

static uint8* s_lastCode = nullptr; // the recompiled program's x86 code (JT_TRACE crash reports)

static void onCrash(int sig, siginfo_t* info, void* context)
{
	char text[512];
	int n = snprintf(text, sizeof(text), "\n*** signal %d while compiling %08x, backtrace:\n", sig, s_currentFunction);
	write(2, text, n);
	const auto* mc = &((ucontext_t*)context)->uc_mcontext;
	const uint64 rip = (uint64)mc->gregs[REG_RIP];
	n = snprintf(text, sizeof(text), "rip %llx (code +%llx), fault address %p, rsp %llx rax %llx rcx %llx rdx %llx rbx %llx rbp %llx rsi %llx rdi %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
		(unsigned long long)rip, (unsigned long long)(rip - (uint64)s_lastCode), info->si_addr,
		(unsigned long long)mc->gregs[REG_RSP], (unsigned long long)mc->gregs[REG_RAX], (unsigned long long)mc->gregs[REG_RCX],
		(unsigned long long)mc->gregs[REG_RDX], (unsigned long long)mc->gregs[REG_RBX], (unsigned long long)mc->gregs[REG_RBP],
		(unsigned long long)mc->gregs[REG_RSI], (unsigned long long)mc->gregs[REG_RDI], (unsigned long long)mc->gregs[REG_R8],
		(unsigned long long)mc->gregs[REG_R9], (unsigned long long)mc->gregs[REG_R10], (unsigned long long)mc->gregs[REG_R11],
		(unsigned long long)mc->gregs[REG_R12], (unsigned long long)mc->gregs[REG_R13], (unsigned long long)mc->gregs[REG_R14],
		(unsigned long long)mc->gregs[REG_R15]);
	write(2, text, n);
	void* frames[64];
	const int count = backtrace(frames, 64);
	backtrace_symbols_fd(frames, count, 2);
	_exit(128 + sig);
}

int main(int argc, char** argv)
{
	// on an alternate stack, so a stack overflow is reported too
	static char altStack[1 << 16];
	stack_t ss{};
	ss.ss_sp = altStack;
	ss.ss_size = sizeof(altStack);
	sigaltstack(&ss, nullptr);
	struct sigaction sa{};
	sa.sa_sigaction = onCrash;
	sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	const uint64 seed = argc > 1 ? strtoull(argv[1], nullptr, 0) : 1;
	const int count = argc > 2 ? atoi(argv[2]) : 1000;
	const int length = argc > 3 ? atoi(argv[3]) : 24;
	const bool verbose = argc > 4 && atoi(argv[4]) != 0;
	// the PS4's Jaguar: AVX and MOVBE, no BMI2 / AVX2
	g_CPUFeatures.x86.bmi2 = false;
	g_CPUFeatures.x86.avx2 = false;
	g_CPUFeatures.x86.fastPext = false;
	g_CPUFeatures.x86.avx = true;
	g_CPUFeatures.x86.movbe = true;
	g_CPUFeatures.x86.lzcnt = true;
	g_CPUFeatures.x86.sse4_1 = true;
	g_CPUFeatures.x86.ssse3 = true;
	// guest memory: a 4 GiB reservation, code and data parts usable
	memory_base = (uint8*)mmap(nullptr, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	mprotect(memory_base + kCode, 0x01000000, PROT_READ | PROT_WRITE);
	mprotect(memory_base + kData, kDataSize, PROT_READ | PROT_WRITE);
	s_global = (PPCInterpreterGlobal_t*)calloc(1, 0x10000);
	// the recompiler's instance data and jump table, as PPCRecompiler_init does
	// (with room for the Wii code's slots, MEM1 0x80000000-0x81800000, 4 GiB above it)
	ppcRecompilerInstanceData = PPCRecompiler_reserveInstanceData();
	if (!ppcRecompilerInstanceData)
	{
		fprintf(stderr, "the recompiler's instance data can't be reserved\n");
		return 1;
	}
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
	PPCRecompiler_allocateRange(kCode, 0x01000000);
	// the jump table's first slots: recompiled code leaves through slot 0 (cycle checks, LEAVE)
	PPCRecompiler_allocateRange(0, 0x1000);
	PPCRecompiler_initPlatform();
	// the PS4's float mode: flush-to-zero and denormals-are-zero
	_mm_setcsr(0x9FC0);
	s_cachePath = getenv("JT_CACHE");
	// after the entry and exit code is made, as on the console (Crema's settings come later there)
	if (const char* eabi = getenv("JT_EABI_RETURNS"))
		g_crema_eabi_returns = atoi(eabi) != 0;
	if (const char* singles = getenv("JT_SINGLE_LOADS"))
		g_crema_single_loads_known = atoi(singles) != 0;
	if (const char* returns = getenv("JT_HOST_RETURNS"); returns && &g_crema_host_returns)
		g_crema_host_returns = atoi(returns) != 0;
	if (getenv("JT_CYCLES_MEMORY"))
		g_crema_cycles_in_register = false;
	// SSA_RUN=1: the Skylanders runtime (ssa_run.cpp) instead of the tests
	if (getenv("SSA_RUN"))
	{
		extern int ssaRun(PPCInterpreterGlobal_t* global);
		return ssaRun(s_global);
	}
	if (const char* rpx = getenv("JT_RPX"))
		return runRpx(rpx, getenv("JT_FUNCS"));
	// only the programs' code and data are mapped (stubs.cpp: memory_isAddressRangeAccessible; the
	// compiled code cache hashes the bytes around a function where they are mapped)
	extern int g_jtMapped;
	extern uint32 g_jtMappedAreas[4][2];
	g_jtMapped = 1;
	const uint32 programAreas[4][2] = {{kCode, kCode + 0x01000000}, {kData, kData + kDataSize}, {0, 0}, {0, 0}};
	memcpy(g_jtMappedAreas, programAreas, sizeof(programAreas));

	if (const char* ops = getenv("JT_OPS"))
		for (const char* p = ops; *p;)
		{
			s_ops.push_back((uint32)strtoul(p, (char**)&p, 10));
			while (*p == ',')
				p++;
		}
	s_singles = getenv("JT_SINGLES") != nullptr;
	s_noBranch = getenv("JT_NOBRANCH") != nullptr;
	if (const char* bench = getenv("JT_BENCH"))
		s_bench = atoi(bench);
	if (const char* regs = getenv("JT_REGS"))
		s_regs = (uint32)atoi(regs);
	s_loops = getenv("JT_LOOPS") != nullptr;
	if (const char* entries = getenv("JT_ENTRIES"))
		s_entries = atoi(entries);
	s_cut = getenv("JT_CUT") != nullptr;
	rng.seed(seed);
	if (getenv("JT_AOT"))
		return runAotTest(count, length, verbose);
	int mismatches = 0, failed = 0;
	int entryRuns[kEntryKinds] = {}, entryMismatches[kEntryKinds] = {}, entriesMissing = 0;
	uint64 total = 0;
	for (int t = 0; t < count; t++)
	{
		Program prog = makeProgram(length);
		State initial;
		initState(initial);
		for (size_t i = 0; i < prog.code.size(); i++)
			*(uint32*)(memory_base + kCode + i * 4) = _swapEndianU32(prog.code[i]);
		// the extra entries (JT_ENTRIES): mostly loop heads, places in loops and branch targets
		std::vector<std::pair<uint32, EntryKind>> extra;
		for (int e = 0; e < s_entries && !prog.entryCandidates.empty(); e++)
		{
			auto pick = prog.entryCandidates[rnd((uint32)prog.entryCandidates.size())];
			if (pick.second == kEntryAny && rnd(4) != 0)
				pick = prog.entryCandidates[rnd((uint32)prog.entryCandidates.size())];
			if (pick.first == 0 || std::any_of(extra.begin(), extra.end(), [&](const auto& x) { return x.first == pick.first; }))
				continue;
			extra.push_back(pick);
		}
		std::set<uint32> entryAddresses{kCode};
		for (auto& [word, kind] : extra)
			entryAddresses.insert(kCode + word * 4);
		// JT_ONLY=test: only that program is compiled and run (the others are just generated)
		if (const char* only = getenv("JT_ONLY"); only && atoi(only) != t)
			continue;
		std::vector<std::pair<MPTR, uint32>> entryPoints;
		PPCFunctionBoundaryTracker::PPCRange_t range;
		std::vector<PPCFunctionBoundaryTracker::PPCRange_t> tracked;
		// JT_CUT: nothing mapped after a word picked per program while it compiles, so the boundary
		// tracker ends the function there; what goes on past it leaves through the jump table
		// (PPCRecompiler_GenerateIML) and the run finishes in the interpreter, as on the console
		const uint32 cutWords = s_cut && prog.code.size() > 3 ? 2 + (uint32)(((uint64)t * 2654435761u) % (prog.code.size() - 2)) : 0;
		if (cutWords)
			g_jtMappedAreas[0][1] = kCode + cutWords * 4;
		PPCRecFunction_t* func = compileProgram(entryAddresses, entryPoints, range, tracked);
		g_jtMappedAreas[0][1] = kCode + 0x01000000;
		if (func && s_cachePath && !throughCache(func, entryPoints, range, tracked, (uint32)prog.code.size(), t))
		{
			s_cacheFailures++;
			delete func;
			func = nullptr; // counted as a failed compile below
		}
		if (func)
		{
			s_codeBytes += func->x86Size;
			s_lastCode = (uint8*)func->x86Code;
			if (getenv("JT_TRACE"))
			{
				printf("%d x86 code %p, %u bytes, entry points:", t, func->x86Code, func->x86Size);
				for (auto& [ppc, x86] : entryPoints)
					printf(" %08x=+%x", ppc, x86);
				printf("\n");
				if (const char* path = getenv("JT_CODE"))
				{
					// JT_CODE=file: the x86 code, for objdump -D -b binary -mi386:x86-64
					if (FILE* f = fopen(path, "wb"))
					{
						fwrite(func->x86Code, 1, func->x86Size, f);
						fclose(f);
					}
				}
			}
		}
		// from the start, then from each extra entry, both from the same state
		for (int e = -1; e < (int)extra.size(); e++)
		{
			const uint32 start = e < 0 ? kCode : kCode + extra[e].first * 4;
			State itp = initial;
			memcpy(memory_base + kData, initial.data.data(), kDataSize);
			_mm_setcsr(0x9FC0);
			runInterpreter(&itp.cpu, start);
			std::vector<uint8> itpData(memory_base + kData, memory_base + kData + kDataSize);
			const uint64 hi = hashState(itp.cpu, itpData.data());
			State jit = initial;
			memcpy(memory_base + kData, initial.data.data(), kDataSize);
			_mm_setcsr(0x9FC0);
			if (getenv("JT_TRACE"))
			{
				// JT_TRACE: what runs next (a crash in the recompiled code shows which)
				printf("%d run from %08x, entries:", t, start);
				for (uint32 a : entryAddresses)
					printf(" %08x", a);
				printf("\n");
				if (e < 0)
					disassemble(prog);
				fflush(stdout);
			}
			const bool ok = func && runRecompiled(&jit.cpu, func, entryPoints, start);
			if (ok && cutWords && jit.cpu.instructionPointer != kReturn)
				runInterpreter(&jit.cpu, jit.cpu.instructionPointer);
			std::vector<uint8> jitData(memory_base + kData, memory_base + kData + kDataSize);
			const uint64 hj = hashState(jit.cpu, jitData.data());
			// JT_CYCLES: the cycles left after each recompiled run (the same with either Cycle Counter)
			if (ok && getenv("JT_CYCLES"))
				fprintf(stderr, "cycles %d %08x %d\n", t, start, jit.cpu.remainingCycles);
			if (e >= 0)
			{
				if (!func)
					continue;
				if (!ok)
				{
					entriesMissing++; // not among the compile's entry points
					continue;
				}
				const EntryKind kind = extra[e].second;
				entryRuns[kind]++;
				if (hi != hj)
				{
					entryMismatches[kind]++;
					printf("%d entry %08x (%s) %016llx %016llx DIFF\n", t, start, kEntryKindNames[kind], (unsigned long long)hi, (unsigned long long)hj);
					if (verbose)
					{
						describe("itp/jit", itp.cpu, jit.cpu, itpData.data(), jitData.data());
						printf("    entries:");
						for (uint32 a : entryAddresses)
							printf(" %08x", a);
						printf("\n");
						disassemble(prog);
					}
				}
				continue;
			}
			if (!ok)
				failed++;
			printf("%d %016llx %016llx %s\n", t, (unsigned long long)hi, (unsigned long long)hj, !ok ? "compile-failed" : hi == hj ? "same" : "DIFF");
			if (const char* dump = getenv("JT_DUMP"); dump && atoi(dump) == t)
			{
				disassemble(prog);
				for (int i = 0; i < 32; i++)
				{
					uint64 b0, b1;
					memcpy(&b0, &jit.cpu.fpr[i].fp0, 8);
					memcpy(&b1, &jit.cpu.fpr[i].fp1, 8);
					printf("  f%d %016llx %016llx\n", i, (unsigned long long)b0, (unsigned long long)b1);
				}
				for (int i = 0; i < 32; i++)
					printf("  r%d %08x\n", i, jit.cpu.gpr[i]);
				for (int i = 0; i < 32; i++)
					printf("  cr%d %d\n", i, jit.cpu.cr[i]);
				for (uint32 i = 0; i < kDataSize; i += 4)
					if (memcmp(&jitData[i], &initial.data[i], 4) != 0)
						printf("  mem +%04x %08x\n", i, _swapEndianU32(*(uint32*)&jitData[i]));
			}
			if (ok && hi != hj)
			{
				mismatches++;
				if (verbose)
				{
					describe("itp/jit", itp.cpu, jit.cpu, itpData.data(), jitData.data());
					if (!extra.empty())
					{
						printf("    entries:");
						for (uint32 a : entryAddresses)
							printf(" %08x", a);
						printf("\n");
					}
					disassemble(prog);
				}
			}
			total += hj;
		}
		delete func;
	}
	fprintf(stderr, "tests %d, interpreter/recompiler differ %d, compile failed %d, total %016llx\n", count, mismatches, failed, (unsigned long long)total);
	if (s_entries)
	{
		fprintf(stderr, "extra entries:");
		for (int k = 0; k < kEntryKinds; k++)
			fprintf(stderr, " %s %d/%d differ", kEntryKindNames[k], entryMismatches[k], entryRuns[k]);
		fprintf(stderr, ", %d not among the entry points\n", entriesMissing);
	}
	if (s_cachePath)
		fprintf(stderr, "compiled code cache: %d programs failed to go through it (counted as failed compiles)\n", s_cacheFailures);
	fprintf(stderr, "x86 code %llu bytes for %d compiled programs", (unsigned long long)s_codeBytes, count - failed);
	if (s_benchRuns)
		fprintf(stderr, ", %.1f TSC ticks per run (incl. state copy)", (double)s_benchTicks / s_benchRuns);
	fprintf(stderr, "\n");
	return 0;
}
