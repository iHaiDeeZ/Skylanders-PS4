// Skylanders: Spyro's Adventure (Wii) runtime, v0: bring-up on the PC host (WSL), inside the jittest
// harness (Crema's recompiler, interpreter and translated-code loader, with stubs for the rest).
//
//   SSA_RUN=1 SSA_DOL=main.dol SSA_AOT=ssa.aot SSA_DISC=<extracted DATA dir> jittest
//
// What it does: Wii memory (MEM1 at 0x80000000 + its uncached mirror 0xC0000000, MEM2 at 0x90000000
// + 0xD0000000), the DOL loaded, low memory as IOS56 + the boot program + the apploader leave it
// (values from Dolphin's Boot_BS2Emu.cpp / IOS.cpp / VersionInfo.cpp), the translated code file
// loaded into the jump table, then execution from __start: translated code where there is some, the
// interpreter in between (unsupported instructions, addresses with no translated code).
//
// Hardware isn't there yet: the hardware register areas (0xCC000000, 0xCD000000, ...) are left
// unmapped, so the first access faults; the fault is reported (guest address, host rip, where the
// guest was) and the run stops. That is the point of v0: how far the game gets, and what it asks for.
//
// SSA_SECONDS: stop after that many seconds (default 20); a status line every second.

#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Recompiler/BackendC/AotFormat.h"
#include <sys/mman.h>
#include <sys/time.h>
#include <signal.h>
#include <setjmp.h>
#ifndef SSA_PS4
#include <execinfo.h>
#endif
#include <unistd.h>
#include <thread>
#include <atomic>
#include "host_ctx.h"
#include "settings.h"
#include <string>
#include <vector>
#include <chrono>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>

extern uint8* memory_base;
void PPCInterpreterWii_executeInstruction(PPCInterpreter_t* hCPU);
void discReadPartition(uint64_t offset, uint32_t length, uint8_t* out); // disc.cpp
void gxReport(); // gx.cpp
void portalUpdate(uint64_t timeBase); // portal.cpp // patch-crema-wii.py: supervisor mode, no MMU
bool CremaAotTestLoad(const std::string& path, uint32 counts[4], std::string& why);
bool mmioEmulate(void* context, uint32_t addr, uint32_t guestIp, void* hostAt); // mmio.cpp
bool mmioTrap(void* context);
uint64_t mmioAccessCount();

// the Wii's time base: 60.75 MHz (bus 243 MHz / 4), from the host's monotonic clock, starting at 0
// the guest address the dispatcher last ran from (fault and trap reports)
static volatile uint32 s_lastIp = 0;

namespace coreinit { extern uint64 (*g_ssaTimeBase)(); }
void aiSrcInitNative(); // mmio.cpp
bool axfxReverbHiNative(uint32 bufs, uint32 rv); // dsp.cpp: SSPE52's AXFX reverb
std::vector<std::pair<uint32, uint32>> axfxReverbHiRegions(uint32 bufs, uint32 rv);
extern volatile uint32_t g_ssaKick; // mmio.cpp: a hardware write since the loop last polled
extern volatile uint64_t g_ssaTicksGx, g_ssaTicksGpuWait; // gx.cpp
uint64_t gxFrameCount();                                  // gx.cpp
void mmioHistReport();                                    // mmio.cpp (SSA_MMIO_HIST=1)
uint32_t mmioHwRead(uint32_t addr, uint32_t size);        // mmio.cpp: translated code's hardware
void mmioHwWrite(uint32_t addr, uint32_t value, uint32_t size); // accesses, called directly
uint64_t mmioDirectCount();
extern uint32 (*g_cremaAotHwRead)(uint32 address, uint32 size); // patch-aot-hw.py
extern void (*g_cremaAotHwWrite)(uint32 address, uint32 value, uint32 size);
extern uint32 (*g_cremaAotSys)(uint8* h, uint32 at); // patch-aot-sys.py
extern uint8* g_cremaAotGp;                           // patch-aot-gp.py
extern uint32* g_cremaAotGpFill;
extern void (*g_cremaAotGpBurst)();
void mmioGatherBurst();                               // mmio.cpp: the gather buffer, its fill, a burst
uint8_t* mmioGatherBuffer();
uint32_t* mmioGatherFill();
bool mmioGatherInline();
uint64_t mmioGatherBursts();
volatile uint32_t g_ssaHwIp = 0; // the guest address the dispatcher last entered (mmio.cpp's logs)
void PPCRecompiler_disableAt(uint32 address); // patch-crema-wii.py
// the Wii's time base (60.75 MHz) from the host clock
static uint64 clockTimeBase()
{
	static const auto start = std::chrono::steady_clock::now();
	const uint64 ns = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
	return ns / 4000 * 243 + (ns % 4000) * 243 / 4000;
}
// ... from the time-stamp counter: a clock read is a system call on the PS4, and the devices' polls,
// the decrementer and the game's time reads made ~50,000 a second. The counter's rate is measured
// against the clock over the first quarter second (the clock until then) and again every 10 s over
// the whole run; the time base never goes back (SSA_CLOCK_TSC=0: the clock every time)
static uint64 ssaTimeBaseRaw()
{
	static const bool tsc = !getenv("SSA_CLOCK_TSC") || atoi(getenv("SSA_CLOCK_TSC"));
	if (!tsc)
		return clockTimeBase();
	static uint64 calTsc = 0, calBase = 0, anchorTsc = 0, anchorBase = 0, last = 0;
	static double perTick = 0; // time-base units per counter tick
	static uint64 spanTicks = 0; // (10 s in counter ticks, made with perTick: no division per read)
	const uint64 t = __rdtsc();
	if (t - anchorTsc < spanTicks)
	{
		const uint64 v = anchorBase + (uint64)((double)(t - anchorTsc) * perTick);
		return last = v > last ? v : last;
	}
	const uint64 base = clockTimeBase();
	if (!calTsc)
	{
		calTsc = t;
		calBase = base;
	}
	else if (base - calBase >= 60750000 / 4)
	{
		const uint64 computed = perTick > 0 ? anchorBase + (uint64)((double)(t - anchorTsc) * perTick) : base;
		perTick = (double)(base - calBase) / (double)(t - calTsc);
		spanTicks = perTick > 0 ? (uint64)(60750000.0 * 10 / perTick) : 0;
		anchorTsc = t;
		anchorBase = computed > base ? computed : base;
	}
	return last = base > last ? base : last;
}
// the time the game spent paused (the quick menu), in time-base units: taken off its clock, so after a
// pause it goes on where it stopped (no catching up)
static uint64 s_tbPaused = 0;
static uint64 ssaTimeBase()
{
	return ssaTimeBaseRaw() - s_tbPaused;
}

bool wiiInterruptPending(); // mmio.cpp
extern std::atomic<int> g_ssaPaused; // overlay.cpp: the quick menu is open
void overlaySetLoads(float game, float gx, float gpuWait); // overlay.cpp: the detailed frame counter's
void gxIdlePresent(); // gx.cpp: a present while the game is paused (no GX thread)
void mmioFlushFifo(); // mmio.cpp: the batched GX FIFO bursts handed on
uint32_t wiiPendingCauses();
static uint64 s_causeCounts[32] = {};
void iosPoll();             // ios.cpp
void viTick(uint64_t timeBase); // mmio.cpp
void btUpdate(uint64_t timeBase); // bt.cpp
void dspUpdate(uint64_t timeBase); // dsp.cpp
uint64_t dspAxFrames();
uint64_t dspDmaBuffers();

// ---- exceptions ----
// Taken between runs, as the 750CL does: SRR0 = where execution goes on, SRR1 = MSR, then MSR with
// EE/PR/FP/FE0/SE/BE/FE1/IR/DR/RI/POW cleared and execution at the vector's physical address (the
// OS copied its handlers there at boot; with translation off they run at 0x00000500 etc., which is
// MEM1's physical alias).
constexpr uint32 kMsrEE = 0x8000, kMsrFP = 0x2000, kMsrIRDR = 0x0030;
static uint64 s_externalTaken = 0, s_decTaken = 0;

static void takeException(PPCInterpreter_t& cpu, uint32 vector, uint64& count)
{
	if (count < 8)
		fprintf(stderr, "ssa: exception %03x taken at %08x (msr %08x)\n", vector, cpu.instructionPointer, cpu.sprExtended.msr);
	cpu.sprExtended.srr0 = cpu.instructionPointer;
	cpu.sprExtended.srr1 = cpu.sprExtended.msr & 0x87C0FFFF;
	cpu.sprExtended.msr &= ~(0x8000u | 0x4000u | 0x2000u | 0x0800u | 0x0400u | 0x0200u | 0x0100u | 0x0030u | 0x0002u | 0x00040000u);
	cpu.instructionPointer = vector;
	count++;
}

// ---- FPU ownership: the OS switches FPU registers lazily. A context switch clears MSR.FP (unless the
// new context owns the FPU), and the first FP instruction traps (0x800) to OSSwitchFPUContext, which
// saves the owner's FPRs into its context, loads the current context's and makes it the owner.
// Translated code can't trap per instruction, so the runtime does that switch itself as soon as
// translated-mode code runs with FP off: like a trap at that point, minus the trap's GPR save into the
// current context (which is what makes it safe anywhere, also in interrupt handlers before they move
// to their own context). Without it threads shared FPRs, and an interrupted lfd/stfd copy wrote
// another thread's floats (or an interrupt callback's zeros) into its data.
extern uint32 g_wiiHID2; // (patch-crema-wii.py)
constexpr uint32 kOsCurrentContext = 0x800000D4, kOsFpuContext = 0x800000D8;
constexpr uint32 kCtxFpr = 0x90, kCtxFpscr = 0x190, kCtxSrr1 = 0x19C, kCtxState = 0x1A2, kCtxPsf = 0x1C8;
static uint64 s_fpuSwitches = 0;

static uint8* guestPtr(uint32 a)
{
	return memory_base + a;
}
static uint32 g32(uint32 a)
{
	return _swapEndianU32(*(uint32*)guestPtr(a));
}
static void p32(uint32 a, uint32 v)
{
	*(uint32*)guestPtr(a) = _swapEndianU32(v);
}
static uint64 g64(uint32 a)
{
	return ((uint64)g32(a) << 32) | g32(a + 4);
}
static void p64(uint32 a, uint64 v)
{
	p32(a, (uint32)(v >> 32));
	p32(a + 4, (uint32)v);
}
static bool contextOk(uint32 c)
{
	return (c >= 0x80000000 && c < 0x81800000 - 0x2C8) || (c >= 0x90000000 && c < 0x94000000 - 0x2C8);
}

static bool fpUnavailable(const PPCInterpreter_t& cpu)
{
	return !(cpu.sprExtended.msr & kMsrFP) && (cpu.sprExtended.msr & kMsrIRDR) == kMsrIRDR &&
		   *(const uint32*)(memory_base + 0x80000800u) != 0; // (the OS's vectors are in)
}

// __OSSaveFPUContext / __OSLoadFPUContext: FPRs at 0x90, FPSCR at 0x190, paired singles' second halves
// (with HID2.PSE) as psq_st pairs at 0x1C8, state bit 0 = FPRs saved
static void fpuSave(PPCInterpreter_t& cpu, uint32 c)
{
	const bool pse = (g_wiiHID2 >> 29) & 1;
	*(uint16*)guestPtr(c + kCtxState) |= _swapEndianU16(1);
	for (int n = 0; n < 32; n++)
	{
		p64(c + kCtxFpr + n * 8, cpu.fpr[n].fp0int);
		if (pse)
		{
			const float a = (float)cpu.fpr[n].fp0, b = (float)cpu.fpr[n].fp1;
			p32(c + kCtxPsf + n * 8, *(const uint32*)&a);
			p32(c + kCtxPsf + n * 8 + 4, *(const uint32*)&b);
		}
	}
	p64(c + kCtxFpscr, 0xFFF8000000000000ull | cpu.fpscr);
}

static void fpuLoad(PPCInterpreter_t& cpu, uint32 c)
{
	if (!(_swapEndianU16(*(uint16*)guestPtr(c + kCtxState)) & 1))
		return; // (never saved: the OS leaves the registers as they are)
	const bool pse = (g_wiiHID2 >> 29) & 1;
	cpu.fpscr = g32(c + kCtxFpscr + 4);
	for (int n = 0; n < 32; n++)
	{
		if (pse)
		{
			const uint32 b = g32(c + kCtxPsf + n * 8 + 4);
			cpu.fpr[n].fp1 = (double)*(const float*)&b;
		}
		cpu.fpr[n].fp0int = g64(c + kCtxFpr + n * 8);
	}
}

// OSSwitchFPUContext without the trap
static void fpuSwitch(PPCInterpreter_t& cpu)
{
	cpu.sprExtended.msr |= kMsrFP;
	const uint32 cur = g32(kOsCurrentContext), owner = g32(kOsFpuContext);
	if (!contextOk(cur))
		return;
	s_fpuSwitches++;
	p32(cur + kCtxSrr1, g32(cur + kCtxSrr1) | kMsrFP);
	p32(kOsFpuContext, cur);
	if (owner == cur)
		return;
	if (contextOk(owner))
		fpuSave(cpu, owner);
	fpuLoad(cpu, cur);
}

// ---- the decrementer: counts down at the time base's rate from the value last written; its
// exception is due once when it passes zero ----
extern uint32 (*g_wiiDecRead)();       // interpreter hooks (patch-crema-wii.py)
extern void (*g_wiiDecWrite)(uint32);
static uint32 s_decValue = 0;
static uint64 s_decStart = 0;
static bool s_decArmed = false;
static uint64 ssaTimeBase();

static uint32 decRead()
{
	return s_decValue - (uint32)(ssaTimeBase() - s_decStart);
}

static void decWrite(uint32 value)
{
	static int s_decLogs = 0;
	if (s_decLogs++ < 5)
		fprintf(stderr, "ssa: decrementer written: %08x\n", value);
	s_decValue = value;
	s_decStart = ssaTimeBase();
	// (written with bit 31 clear: armed to fire when it goes negative)
	s_decArmed = (sint32)value >= 0;
}

static bool decrementerDue()
{
	if (!s_decArmed || (sint32)decRead() >= 0)
		return false;
	s_decArmed = false;
	return true;
}

// due, without taking it (aotSys: the loop takes it)
static bool decrementerPending()
{
	return s_decArmed && (sint32)decRead() < 0;
}

// The OS's threads (RVL SDK OSThread: context first, state at 0x2C8, priority 0x2D0, the queue it
// waits on at 0x2DC, active-list link at 0x2FC; context: gpr[1] at 0x4, lr 0x84, srr0 0x198), from the
// active-thread list at 0x800000DC, each with its saved stack's return addresses (back chain at 0,
// saved LR at +4). The running thread's state comes from the CPU. Names: tools/addr2name.py.
static uint32 rd32(uint32 a)
{
	const bool mapped = (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000);
	return mapped ? _swapEndianU32(*(uint32*)(memory_base + a)) : 0;
}

static void dumpThreads(const PPCInterpreter_t& cpu)
{
	const uint32 current = rd32(0x800000E4);
	uint32 t = rd32(0x800000DC);
	for (int n = 0; t && n < 32; n++, t = rd32(t + 0x2FC))
	{
		const uint16 state = (uint16)(rd32(t + 0x2C8) >> 16);
		const bool running = t == current;
		const uint32 sp = running ? cpu.gpr[1] : rd32(t + 0x4);
		const uint32 pc = running ? cpu.instructionPointer : rd32(t + 0x198);
		const uint32 lr = running ? cpu.spr.LR : rd32(t + 0x84);
		fprintf(stderr, "thread %08x%s state %u prio %d waits on %08x: pc %08x lr %08x stack", t, running ? " (current)" : "", state,
			(int)rd32(t + 0x2D0), rd32(t + 0x2DC), pc, lr);
		uint32 frame = sp;
		for (int i = 0; i < 12 && frame; i++)
		{
			const uint32 next = rd32(frame);
			if (!next || next <= frame)
				break;
			fprintf(stderr, " %08x", rd32(next + 4));
			frame = next;
		}
		fprintf(stderr, "\n");
	}
}

// ---- SSA_WWATCH=address: writes to that guest word (cached 0x8... view of MEM1/MEM2) ----
// The host page is write-protected; a write to it faults, is let through for one instruction (trap
// flag) and the page protected again; a change of the watched word is logged with the code that did it.
extern "C" void ps4_crash_jit_info(int fd, uint64 rip, int brief); // PPCRecompiler.cpp
static uint32 s_wwatch = 0;
static uint8* s_wwatchPage = nullptr;
static uint32 s_wwatchBefore = 0;
static uint64 s_wwatchRip = 0;
static bool s_wwatchStepping = false;
static int s_wwatchLogs = 0;

// SSA_WWATCH=address,length: a range instead; every write to it is counted by the host code that did
// it (the writers are listed at the end, with the guest function for translated code)
static uint32 s_wrangeLo = 0, s_wrangeHi = 0;
static uint8* s_wrangePage = nullptr;
struct WriterCount
{
	uint64 rip;
	uint64 count;
	uint32 firstGuest, lastGuest, guestIp, second;
};
static WriterCount s_writers[512];
static int s_writerCount = 0;
volatile sig_atomic_t s_seconds = 0;

static bool wrangeFault(void* at, void* context)
{
	if (!s_wrangeHi || (uint8*)at < memory_base + s_wrangeLo || (uint8*)at >= memory_base + s_wrangeHi)
		return false;
	const uint64 rip = hostctx::rip(context);
	const uint32 guest = (uint32)((uint8*)at - memory_base);
	int i = 0;
	while (i < s_writerCount && s_writers[i].rip != rip)
		i++;
	if (i == s_writerCount && s_writerCount < 512)
		s_writers[s_writerCount++] = {rip, 0, guest, guest, s_lastIp, (uint32)s_seconds};
	if (i < 512)
	{
		s_writers[i].count++;
		s_writers[i].lastGuest = guest;
	}
	s_wrangePage = (uint8*)((uintptr_t)at & ~(hostctx::kPage - 1));
	hostProtect(s_wrangePage, hostctx::kPage, true);
	hostctx::flags(context) |= 0x100;
	s_wwatchStepping = true;
	return true;
}

static void wrangeReport()
{
	if (!s_wrangeHi)
		return;
	fprintf(stderr, "wwatch: %d writers to %08x-%08x:\n", s_writerCount, s_wrangeLo, s_wrangeHi);
	for (int i = 0; i < s_writerCount; i++)
	{
		const WriterCount& w = s_writers[i];
		fprintf(stderr, "  host %llx: %llu writes, first %08x last %08x, guest ip ~%08x, from %u s\n", (unsigned long long)w.rip,
			(unsigned long long)w.count, w.firstGuest, w.lastGuest, w.guestIp, w.second);
		ps4_crash_jit_info(2, w.rip, 1);
	}
}

static bool wwatchFault(void* at, void* context)
{
	if (wrangeFault(at, context))
		return true;
	if (!s_wwatchPage || (uint8*)at < s_wwatchPage || (uint8*)at >= s_wwatchPage + hostctx::kPage)
		return false;
	s_wwatchBefore = *(uint32*)(memory_base + s_wwatch);
	s_wwatchRip = hostctx::rip(context);
	hostProtect(s_wwatchPage, hostctx::kPage, true);
	hostctx::flags(context) |= 0x100;
	s_wwatchStepping = true;
	return true;
}

static bool wwatchTrap(void* context)
{
	if (!s_wwatchStepping)
		return false;
	s_wwatchStepping = false;
	hostctx::flags(context) &= ~0x100ull;
	if (s_wrangePage)
	{
		hostProtect(s_wrangePage, hostctx::kPage, false);
		s_wrangePage = nullptr;
		return true;
	}
	const uint32 now = *(uint32*)(memory_base + s_wwatch);
	if (now != s_wwatchBefore && s_wwatchLogs++ < 40)
	{
		char line[160];
		const int n = snprintf(line, sizeof(line), "wwatch: %08x: %08x -> %08x by host %llx (guest ip ~%08x lr %08x)\n", s_wwatch,
			_swapEndianU32(s_wwatchBefore), _swapEndianU32(now), (unsigned long long)s_wwatchRip, s_lastIp);
		write(2, line, n);
		ps4_crash_jit_info(2, s_wwatchRip, 1);
	}
	hostProtect(s_wwatchPage, hostctx::kPage, false);
	return true;
}

static void onTrap(int sig, siginfo_t* info, void* context)
{
	if (wwatchTrap(context))
		return;
	if (!mmioTrap(context))
	{
		char line[128];
		const int n = snprintf(line, sizeof(line), "ssa: unexpected SIGTRAP (assert?) at host rip %llx, guest ip %08x\n",
			(unsigned long long)hostctx::rip(context), s_lastIp);
		write(2, line, n);
		_exit(4);
	}
}
void CremaAotTestDirectCalls(bool on);

namespace
{
constexpr uint32 kMem1 = 0x80000000, kMem1Size = 0x01800000;
constexpr uint32 kMem2 = 0x90000000, kMem2Size = 0x04000000;
constexpr uint32 kUncached = 0x40000000; // 0xC0000000 = 0x80000000 + this

PPCInterpreter_t* s_cpu = nullptr;
volatile uint64 s_translatedRuns = 0, s_interpreted = 0;
// time-stamp-counter ticks in translated code and in the interpreter (onSecond's time split)
volatile uint64 g_ssaTicksTranslated = 0, g_ssaTicksInterp = 0;
volatile uint64 s_polls = 0; // the loop's device polls
// the devices' polling: every s_pollTsc time-stamp ticks (SSA_POLL_TSC, default 32768: ~20 us on the
// PS4), from the loop and from aotSys
static uint64 s_pollTsc = 32768, s_lastPollTsc = 0;
// the quick menu open (overlay.cpp sets g_ssaPaused from the input thread): the game waits here, at a
// device poll, with its clock stopped. The GX thread presents the menu over the last frame (without a
// GX thread this loop does).
static void pauseGame()
{
	const uint64 tb0 = ssaTimeBaseRaw();
	fprintf(stderr, "ssa: paused (the quick menu)\n");
	while (g_ssaPaused.load(std::memory_order_acquire))
		gxIdlePresent();
	const uint64 paused = ssaTimeBaseRaw() - tb0;
	s_tbPaused += paused;
	fprintf(stderr, "ssa: resumed after %.1f s\n", paused / 60750000.0);
}
static void pollDevices(uint64 tscNow)
{
	if (g_ssaPaused.load(std::memory_order_relaxed))
	{
		pauseGame();
		tscNow = __rdtsc();
	}
	g_ssaKick = 0;
	s_lastPollTsc = tscNow;
	s_polls++;
	mmioFlushFifo(); // (the batched GX FIFO bursts: to the GX thread at least this often)
	iosPoll();
	const uint64 now = ssaTimeBase();
	viTick(now);
	btUpdate(now);
	portalUpdate(now);
	dspUpdate(now);
}

// (7) a supervisor instruction translated code reached (patch-aot-sys.py: mfmsr/mtmsr, the supervisor
// SPRs, cache, TLB and FPSCR ops): the interpreter runs it here and translated code goes on after it,
// unless the loop has to see something first - an interrupt that MSR.EE now lets in (the devices polled
// first when that is due, as the loop would after a leave), an MSR change other than EE/RI/ME (FP off:
// the FPU switch; translation, privilege), an exception (execution elsewhere). Before, each one was a
// leave: back up the host stack to the loop, then in again at every return address on the way.
// SSA_AOT_SYS=0: leaves, as before.
// (BackendC does the cache/sync hints, mfmsr and most mtmsr inline: what comes here are the other SPR
// and FPSCR moves and mtmsr turning EE on.) SSA_RUNPROF=1: these calls by address too (from
// SSA_PROF_FROM seconds), in the report at the end.
volatile uint64 s_inPlace = 0;
static std::unordered_map<uint32, uint64>* s_sysCounts = nullptr;
static int s_sysProfFrom = 0;
constexpr uint32 kMsrRI = 0x0002, kMsrME = 0x1000;
static uint32 aotSys(uint8* h, uint32 at)
{
	PPCInterpreter_t& cpu = *(PPCInterpreter_t*)h;
	const uint32 msr0 = cpu.sprExtended.msr;
	cpu.instructionPointer = at;
	PPCInterpreterWii_executeInstruction(&cpu);
	s_inPlace = s_inPlace + 1;
	if (s_sysCounts && (int)s_seconds >= s_sysProfFrom)
		(*s_sysCounts)[at]++;
	const uint32 next = cpu.instructionPointer, msr = cpu.sprExtended.msr;
	if (next != at + 4 || ((msr ^ msr0) & ~(kMsrEE | kMsrRI | kMsrME)))
		return next | 1;
	if (msr & ~msr0 & kMsrEE)
	{
		const uint64 tscNow = __rdtsc();
		if (g_ssaKick || tscNow - s_lastPollTsc >= s_pollTsc)
			pollDevices(tscNow);
		if (wiiInterruptPending() || decrementerPending())
		{
			g_ssaKick = 1; // (the loop polls at once and takes it)
			return next | 1;
		}
	}
	return next;
}
// SSPE52's __AI_SRC_INIT (done natively, see aiSrcInitNative)
constexpr uint32 kAiSrcInit = 0x803E3C10;
static bool s_aiSrcNative = false;
// SSPE52's AXFX reverb (the SDK's HandleReverb, dsp.cpp axfxReverbHiNative): ~8% of the game thread as
// translated code; its translation is switched off and the loop runs it natively (SSA_REVERB_NATIVE=0:
// translated)
constexpr uint32 kReverbHi = 0x8037AA70;
constexpr uint32 kReverbJump = 0x8037A770; // (b 0x8037AA70: its translation has the reverb's body in it)
static bool s_reverbNative = false;
static uint64 s_reverbNativeCalls = 0;

// SSA_FIND_ENTRIES=1: untranslated call targets (the entry list's misses), counted; written to the
// log every 10 s as "entry-found <address> <calls>" lines, the busiest first (for the next translation)
static bool s_findEntries = false;
static std::unordered_map<uint32, uint64> s_foundEntries;
// (bit 0 set: a place interpretation started, kept apart from call targets in the report: "start")
static void noteEntry(uint32 address)
{
	const PPCREC_JUMP_ENTRY s = ((const PPCREC_JUMP_ENTRY*)(void*)ppcRecompilerInstanceData)[(address & ~3u) / 4];
	if (s && s != PPCRecompiler_leaveRecompilerCode_unvisited && s != PPCRecompiler_leaveRecompilerCode_visited)
		return; // (translated: entered and left at its first instruction)
	s_foundEntries[address]++;
}
static void reportEntries()
{
	std::vector<std::pair<uint64, uint32>> v;
	for (auto& [a, n] : s_foundEntries)
		v.push_back({n, a});
	std::sort(v.rbegin(), v.rend());
	fprintf(stderr, "ssa: %zu untranslated call targets so far\n", v.size());
	for (auto& [n, a] : v)
		fprintf(stderr, "entry-found %08x %llu%s\n", a & ~3u, (unsigned long long)n, (a & 1) ? " start" : "");
}

void put32(uint32 a, uint32 v) { *(uint32*)(memory_base + a) = _swapEndianU32(v); }
void put16(uint32 a, uint16 v) { *(uint16*)(memory_base + a) = _swapEndianU16(v); }
void put8(uint32 a, uint8 v) { memory_base[a] = v; }

std::vector<uint8> readFile(const std::string& path)
{
	std::vector<uint8> data;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		fseek(f, 0, SEEK_END);
		data.resize(ftell(f));
		fseek(f, 0, SEEK_SET);
		if (fread(data.data(), 1, data.size(), f) != data.size())
			data.clear();
		fclose(f);
	}
	return data;
}

// one block of memory seen at three guest addresses, as the Wii's fixed BATs show it: physical (code
// run in real mode, translation off), cached (0x8/0x9...) and uncached (0xC/0xD...)
bool mapMirrored(uint32 guest, uint32 size, const char* name)
{
	const uint32 views[3] = {guest & 0x3FFFFFFFu, guest, guest + kUncached};
	return hostMapMirrored(memory_base, views, 3, size);
}

// the DOL's sections in MEM1, its BSS cleared; the entry point
bool loadDol(const std::vector<uint8>& dol, uint32& entry)
{
	if (dol.size() < 0x100)
		return false;
	auto be = [&](size_t o) { return _swapEndianU32(*(const uint32*)(dol.data() + o)); };
	for (int i = 0; i < 18; i++)
	{
		const uint32 offset = be(i * 4), address = be(0x48 + i * 4), size = be(0x90 + i * 4);
		if (!size)
			continue;
		if (offset + size > dol.size() || address < kMem1 || address + size > kMem1 + kMem1Size)
			return false;
		memcpy(memory_base + address, dol.data() + offset, size);
	}
	const uint32 bss = be(0xD8), bssSize = be(0xDC);
	// (the BSS range covers the small data sections too: clear only what no section loaded)
	std::vector<std::pair<uint32, uint32>> loaded;
	for (int i = 0; i < 18; i++)
		if (be(0x90 + i * 4))
			loaded.emplace_back(be(0x48 + i * 4), be(0x48 + i * 4) + be(0x90 + i * 4));
	for (uint32 a = bss; a < bss + bssSize; a += 4)
	{
		bool inSection = false;
		for (auto& [s, e] : loaded)
			inSection |= a >= s && a < e;
		if (!inSection)
			put32(a, 0);
	}
	entry = be(0xE0);
	return true;
}

// low memory as the system menu's IOS, the boot program (BS2), IOS56 and the apploader leave it
// (Dolphin: IOS.cpp SetupMemory Full + Boot_BS2Emu.cpp SetupWiiMemory + IOSReload; VersionInfo.cpp IOS56)
// ---- the apploader (Dolphin's CBoot::RunApploader): its image (after a 0x20-byte header) at
// 0x81200000; the entry point gives init/main/close at 0x80004000; main asks for one disc read per
// call (RAM address, length, partition offset >> 2 at 0x81300004/8/C) until it returns 0; close
// returns the game's entry point. OSReport is a blr at 0x81300000. It places the FST below the top
// of MEM1, BI2 0x2000 below the FST, and lowers the arena to the FST. (A fixed BI2 address put the
// argument block __start lowers the arena to ~380 KB too low: the game's memory pools ran out.)

static bool runApploader(PPCInterpreter_t& cpu, const std::string& disc, uint32& entry)
{
	const std::vector<uint8> img = readFile(disc + "/sys/apploader.img");
	if (img.size() < 0x20)
	{
		fprintf(stderr, "ssa: no apploader.img: the runtime's own FST/BI2 placement stays\n");
		return false;
	}
	auto be = [&](size_t o) { return (uint32)img[o] << 24 | img[o + 1] << 16 | img[o + 2] << 8 | img[o + 3]; };
	const uint32 start = be(0x10), size = be(0x14) + be(0x18);
	memcpy(memory_base + 0x81200000u, img.data() + 0x20, std::min<size_t>(size, img.size() - 0x20));
	auto run = [&](uint32 address) {
		cpu.instructionPointer = address;
		cpu.spr.LR = 0;
		for (uint64 n = 0; cpu.instructionPointer != 0; n++)
		{
			if (n > 200000000)
			{
				fprintf(stderr, "ssa: apploader stuck at %08x\n", cpu.instructionPointer);
				return false;
			}
			cpu.remainingCycles = 1000;
			PPCInterpreterWii_executeInstruction(&cpu);
		}
		return true;
	};
	cpu.gpr[3] = 0x80004000;
	cpu.gpr[4] = 0x80004004;
	cpu.gpr[5] = 0x80004008;
	if (!run(start))
		return false;
	const uint32 init = rd32(0x80004000), main = rd32(0x80004004), close = rd32(0x80004008);
	*(uint32*)(memory_base + 0x81300000u) = _swapEndianU32(0x4E800020); // (OSReport)
	cpu.gpr[3] = 0x81300000;
	if (!run(init))
		return false;
	int reads = 0;
	for (;;)
	{
		cpu.gpr[3] = 0x81300004;
		cpu.gpr[4] = 0x81300008;
		cpu.gpr[5] = 0x8130000C;
		if (!run(main))
			return false;
		if (cpu.gpr[3] == 0)
			break;
		const uint32 ram = rd32(0x81300004), length = rd32(0x81300008);
		const uint64 offset = (uint64)rd32(0x8130000C) << 2;
		::discReadPartition(offset, length, memory_base + (ram | 0x80000000u));
		reads++;
	}
	if (!run(close))
		return false;
	entry = cpu.gpr[3];
	fprintf(stderr, "ssa: apploader: %d reads, entry %08x; FST %08x size %x, BI2 %08x, arena high %08x, MEM1 arena end %08x\n",
		reads, entry, rd32(0x80000038), rd32(0x8000003C), rd32(0x800000F4), rd32(0x80000034), rd32(0x80003110));
	return true;
}

bool setupLowMemory(const std::string& disc)
{
	const std::vector<uint8> boot = readFile(disc + "/sys/boot.bin");
	const std::vector<uint8> bi2 = readFile(disc + "/sys/bi2.bin");
	const std::vector<uint8> fst = readFile(disc + "/sys/fst.bin");
	if (boot.size() < 0x440 || bi2.size() != 0x2000 || fst.empty())
	{
		fprintf(stderr, "ssa: %s/sys/{boot,bi2,fst}.bin missing\n", disc.c_str());
		return false;
	}
	memset(memory_base + kMem1, 0, 0x4000);
	// the disc header's first 32 bytes (game ID, maker, disc number, version, Wii magic at 0x18)
	memcpy(memory_base + kMem1, boot.data(), 0x20);
	// IOS56 (VersionInfo.cpp)
	const uint32 mem2End = 0x93600000, mem2ArenaEnd = 0x935E0000;
	put32(kMem1 + 0x3100, kMem1Size);          // MEM1 size
	put32(kMem1 + 0x3104, kMem1Size);          // MEM1 simulated size
	put32(kMem1 + 0x3108, 0x81800000);         // MEM1 end
	put32(kMem1 + 0x310C, 0x00000000);         // MEM1 arena begin
	put32(kMem1 + 0x3110, 0x81800000);         // MEM1 arena end
	put32(kMem1 + 0x3114, 0xDEADBEEF);
	put32(kMem1 + 0x3118, kMem2Size);          // MEM2 size
	put32(kMem1 + 0x311C, kMem2Size);          // MEM2 simulated size
	put32(kMem1 + 0x3120, mem2End);            // MEM2 end
	put32(kMem1 + 0x3124, 0x90000800);         // MEM2 arena begin
	put32(kMem1 + 0x3128, mem2ArenaEnd);       // MEM2 arena end
	put32(kMem1 + 0x312C, 0xDEADBEEF);
	put32(kMem1 + 0x3130, mem2ArenaEnd);       // IPC buffer begin
	put32(kMem1 + 0x3134, mem2End);            // IPC buffer end
	put32(kMem1 + 0x3138, 0x00000011);         // Hollywood revision
	put32(kMem1 + 0x313C, 0xDEADBEEF);
	put32(kMem1 + 0x3140, 0x0038161E);         // IOS version (56, v5662)
	put32(kMem1 + 0x3144, 0x00030110);         // IOS date
	put32(kMem1 + 0x3148, 0x93600000);         // IOS reserved begin
	put32(kMem1 + 0x314C, 0x93620000);         // IOS reserved end
	put32(kMem1 + 0x3150, 0xDEADBEEF);
	put32(kMem1 + 0x3154, 0xDEADBEEF);
	put32(kMem1 + 0x3158, 0x0000FF01);         // RAM vendor
	put32(kMem1 + 0x3160, 0x00000000);         // system menu sync
	put32(kMem1 + 0x28, kMem1Size);            // legacy: memory size
	put32(kMem1 + 0xF0, kMem1Size);            // legacy: simulated memory size
	// the boot program (SetupWiiMemory)
	put32(kMem1 + 0x20, 0x0D15EA5E);           // boot magic
	put32(kMem1 + 0x24, 0x00000001);
	put32(kMem1 + 0x2C, 0x00000023);           // board model: RVL retail 3
	put32(kMem1 + 0x30, 0x00000000);           // arena low
	put32(kMem1 + 0x34, 0x817FEC60);           // arena high (the apploader lowers it below the FST)
	put32(kMem1 + 0xE4, 0x8008F7B8);           // thread init
	put32(kMem1 + 0xF4, 0x8179B500);           // BI2 copy
	put32(kMem1 + 0xF8, 0x0E7BE2C0);           // bus clock 243 MHz
	put32(kMem1 + 0xFC, 0x2B73A840);           // CPU clock 729 MHz
	put32(kMem1 + 0x30D8, 0xFFFFFFFF);
	put16(kMem1 + 0x30E6, 0x8201);             // console type
	put8(kMem1 + 0x315C, 0x80);                // OSInit
	put8(kMem1 + 0x315D, 0xAD);                // apploader flag (IOS)
	put16(kMem1 + 0x315E, 0x0113);             // devkit boot program version
	put32(kMem1 + 0x3184, 0x80000000);         // game ID address
	put32(kMem1 + 0x3188, 0x0038161E);         // the apploader's IOS version word (OSInit's IOS check: ERROR #002)
	put32(kMem1 + 0xCC, 0);                    // VI mode as the IPL left it: NTSC
	memcpy(memory_base + kMem1 + 0x3180, boot.data(), 4); // game ID (checked at boot)
	// partition: the data partition's offset >> 2 (BS2: 0x3194/0x3198); filled from the disc later
	put32(kMem1 + 0x3194, 0);
	// the apploader: BI2 copied, the FST at the top of the arena, the arena lowered to it
	memcpy(memory_base + 0x8179B500, bi2.data(), bi2.size());
	const uint32 fstSize = (uint32)fst.size();
	const uint32 fstAddress = (0x817FEC60 - fstSize) & ~0x1Fu;
	memcpy(memory_base + fstAddress, fst.data(), fstSize);
	put32(kMem1 + 0x38, fstAddress);           // FST
	put32(kMem1 + 0x3C, fstSize);              // FST maximum size
	put32(kMem1 + 0x34, fstAddress);           // arena high
	// setting.txt (encrypted) at 0x3800, as the boot program leaves it (the OS reads region/video there)
	if (const char* nand = getenv("SSA_NAND"))
	{
		const std::vector<uint8> setting = readFile(std::string(nand) + "/title/00000001/00000002/data/setting.txt");
		if (setting.size() == 0x100)
			memcpy(memory_base + kMem1 + 0x3800, setting.data(), 0x100);
		else
			fprintf(stderr, "ssa: no setting.txt in %s (tools/make-nand.py makes one)\n", nand);
		// the quick menu's Game Widescreen: SYSCONF's IPL.AR (1: a 16:9 TV), which the game reads at boot
		// (rewritten only when it differs)
		const std::string sysconfPath = std::string(nand) + "/shared2/sys/SYSCONF";
		std::vector<uint8> sysconf = readFile(sysconfPath);
		const uint8 kName[] = {'I', 'P', 'L', '.', 'A', 'R'};
		auto at = std::search(sysconf.begin(), sysconf.end(), kName, kName + 6);
		const uint8 want = ssa::settingAtStart("widescreen") == 0 ? 1 : 0;
		if (at != sysconf.end() && at + 6 < sysconf.end() && *(at + 6) != want)
		{
			*(at + 6) = want;
			if (FILE* f = fopen(sysconfPath.c_str(), "wb"))
			{
				fwrite(sysconf.data(), 1, sysconf.size(), f);
				fclose(f);
				fprintf(stderr, "ssa: SYSCONF IPL.AR = %u (the game's widescreen %s)\n", want, want ? "on" : "off");
			}
		}
	}
	// default exception handlers: rfi
	put32(kMem1 + 0x300, 0x4C000064);
	put32(kMem1 + 0x800, 0x4C000064);
	put32(kMem1 + 0xC00, 0x4C000064);
	fprintf(stderr, "ssa: low memory set up (game %.6s, FST %08x size %x)\n", (const char*)boot.data(), fstAddress, fstSize);
	return true;
}

// a guest fault (hardware registers, unmapped memory): reported, then the run ends
sigjmp_buf s_stop;
volatile sig_atomic_t s_running = 0;
volatile uint32 s_faultGuest = 0;
volatile uint64 s_faultRip = 0;
volatile uint64 s_faultAddr = 0, s_faultRegs[16]; // (the host side of a fault, for the report)
volatile int s_faultSignal = 0;

void onFault(int sig, siginfo_t* info, void* context)
{
	const uint8* at = (const uint8*)info->si_addr;
	// hardware registers: the access emulated (mmio.cpp), execution goes on; through their physical
	// addresses too (0x0C000000: code run in real mode)
	if ((sig == SIGSEGV || sig == SIGBUS) && wwatchFault((void*)at, context))
		return;
	if ((sig == SIGSEGV || sig == SIGBUS) && at >= memory_base && at < memory_base + 0x100000000ull)
	{
		const uint32 guest = (uint32)(at - memory_base);
		const uint32 device = (guest >= 0x0C000000 && guest < 0x0E000000) ? guest | 0xC0000000u : guest;
		if (device >= 0xCC000000 && device < 0xCE000000 && mmioEmulate(context, device, s_lastIp, (void*)at))
			return;
	}
	s_faultSignal = sig;
	s_faultRip = hostctx::rip(context);
	s_faultGuest = (at >= memory_base && at < memory_base + 0x100000000ull) ? (uint32)(at - memory_base) : 0xFFFFFFFFu;
	s_faultAddr = (uint64)at;
	for (int i = 0; i < 16; i++)
		s_faultRegs[i] = hostctx::reg(context, i);
	if (s_running)
	{
		s_running = 0;
		siglongjmp(s_stop, 1);
	}
	_exit(3);
}

void onSecond(int)
{
	s_seconds = s_seconds + 1;
	char line[512];
	int n = snprintf(line, sizeof(line), "ssa: %ds: ip %08x lr %08x, %llu translated runs, %llu interpreted, %llu hardware accesses\n",
		(int)s_seconds, s_lastIp, s_cpu ? s_cpu->spr.LR : 0, (unsigned long long)s_translatedRuns, (unsigned long long)s_interpreted,
		(unsigned long long)mmioAccessCount());
	// the second's time split (SSA_TIMES=0: off): the main thread's share of the second in translated
	// code (its hardware-access traps and the renderer included), the renderer (decode, draw
	// recording, GPU wait), the GPU wait alone, the interpreter; per-second counts
	static const bool times = !getenv("SSA_TIMES") || atoi(getenv("SSA_TIMES"));
	if (times)
	{
		static uint64 lastDirect = 0, lastTsc = 0, lastTr = 0, lastIn = 0, lastGx = 0, lastWait = 0, lastRuns = 0, lastInsns = 0, lastHw = 0, lastFrames = 0, lastInPlace = 0,
			lastBursts = 0;
		const uint64 tsc = __rdtsc(), tr = g_ssaTicksTranslated, in = g_ssaTicksInterp, gxT = g_ssaTicksGx, wait = g_ssaTicksGpuWait;
		const uint64 runs = s_translatedRuns, insns = s_interpreted, hw = mmioAccessCount(), frames = gxFrameCount(), direct = mmioDirectCount(), inPlace = s_inPlace,
			bursts = mmioGatherBursts();
		if (lastTsc)
		{
			const double d = (double)(tsc - lastTsc) / 100.0;
			const uint64 dHw = hw - lastHw, dDirect = direct - lastDirect;
			overlaySetLoads((float)((tr - lastTr) + (in - lastIn)) / (float)d, (float)(gxT - lastGx) / (float)d, (float)(wait - lastWait) / (float)d);
			n += snprintf(line + n, sizeof(line) - n,
				"time: translated %.0f%% (renderer %.0f%%, GPU wait %.0f%%), interpreter %.0f%%; %llu runs, %llu interpreted, %llu in place, %llu hardware accesses (%llu trapped), %llu KiB GX FIFO, %llu XFB copies\n",
				(tr - lastTr) / d, (gxT - lastGx) / d, (wait - lastWait) / d, (in - lastIn) / d, (unsigned long long)(runs - lastRuns),
				(unsigned long long)(insns - lastInsns), (unsigned long long)(inPlace - lastInPlace), (unsigned long long)dHw,
				(unsigned long long)(dHw > dDirect ? dHw - dDirect : 0), (unsigned long long)((bursts - lastBursts) * 32 / 1024), (unsigned long long)(frames - lastFrames));
		}
		lastTsc = tsc, lastTr = tr, lastIn = in, lastGx = gxT, lastWait = wait, lastRuns = runs, lastInsns = insns, lastHw = hw, lastFrames = frames, lastDirect = direct, lastInPlace = inPlace,
			lastBursts = bursts;
	}
	write(2, line, n);
	if (s_seconds % 10 == 0)
		mmioHistReport();
}

const char* blockName(uint32 a)
{
	if (a >= 0xCC000000 && a < 0xCC001000) return "CP (command processor)";
	if (a >= 0xCC001000 && a < 0xCC002000) return "PE (pixel engine)";
	if (a >= 0xCC002000 && a < 0xCC003000) return "VI (video)";
	if (a >= 0xCC003000 && a < 0xCC004000) return "PI (interrupts/FIFO)";
	if (a >= 0xCC004000 && a < 0xCC005000) return "MI (memory)";
	if (a >= 0xCC005000 && a < 0xCC006000) return "DSP";
	if (a >= 0xCC008000 && a < 0xCC009000) return "GX FIFO";
	if (a >= 0xCD000000 && a < 0xCD006000) return "Hollywood (IPC, GPIO, timers)";
	if (a >= 0xCD006000 && a < 0xCD006400) return "DI";
	if (a >= 0xCD006400 && a < 0xCD006800) return "SI";
	if (a >= 0xCD006800 && a < 0xCD006C00) return "EXI";
	if (a >= 0xCD006C00 && a < 0xCD007000) return "AI";
	if (a >= 0xCD800000 && a < 0xCD808000) return "Hollywood mirror";
	if (a >= 0xE0000000 && a < 0xE0004000) return "locked cache";
	return "unmapped memory";
}
// the entry's translated function in memory (0: not found); SSA_AOT_DUMP's reference point
uint64 s_aotEntryFn = 0;

// SSA_AOT_DUMP=file,from,size (the PC's profile-guided builds, recomp/pgo.sh): at the end of the run,
// size bytes of the translated code's image from `from` bytes after the entry's function - an
// instrumented build's counters (__llvm_prf_cnts), which recomp/pgo-profraw.py makes a .profraw of
void aotDump()
{
	const char* e = getenv("SSA_AOT_DUMP");
	if (!e || !s_aotEntryFn)
		return;
	char path[512];
	long long from = 0;
	unsigned long long size = 0;
	if (sscanf(e, "%511[^,],%lli,%lli", path, &from, (long long*)&size) != 3 || size == 0)
	{
		fprintf(stderr, "ssa: SSA_AOT_DUMP=%s: file,from,size wanted\n", e);
		return;
	}
	FILE* f = fopen(path, "wb");
	const size_t n = f ? fwrite((const void*)(s_aotEntryFn + from), 1, (size_t)size, f) : 0;
	if (f)
		fclose(f);
	fprintf(stderr, "ssa: SSA_AOT_DUMP: %zu of %llu bytes to %s\n", n, size, path);
}
} // namespace

int ssaRun(PPCInterpreterGlobal_t* global)
{
	const char* dolPath = getenv("SSA_DOL");
	const char* aotPath = getenv("SSA_AOT");
	const char* disc = getenv("SSA_DISC");
	if (!dolPath || !aotPath || !disc)
	{
		fprintf(stderr, "ssa: SSA_DOL, SSA_AOT and SSA_DISC are needed\n");
		return 1;
	}
#ifndef SSA_PS4
	// (the PC host: the quick menu's settings file, SSA_SETTINGS=path; the PS4's main reads its own)
	if (const char* settings = getenv("SSA_SETTINGS"))
		ssa::settingsLoad(settings);
#endif
	// memory: the harness reserved 4 GiB at memory_base (its Wii U test areas below 0x20000000)
	if (!mapMirrored(kMem1, kMem1Size, "mem1") || !mapMirrored(kMem2, kMem2Size, "mem2"))
	{
		fprintf(stderr, "ssa: guest memory can't be mapped\n");
		return 1;
	}
	// the locked cache: 16 KB of on-chip scratch at 0xE0000000, filled and drained by the DMA in
	// DMAU/DMAL (the interpreter's setDMAL) and used with ordinary loads and stores
	if (!hostMapPlain(memory_base + 0xE0000000u, 0x4000))
	{
		fprintf(stderr, "ssa: the locked cache can't be mapped\n");
		return 1;
	}
	uint32 entry = 0;
	if (!loadDol(readFile(dolPath), entry))
	{
		fprintf(stderr, "ssa: %s can't be loaded\n", dolPath);
		return 1;
	}
	if (!setupLowMemory(disc))
		return 1;
	extern bool discInit(const std::string& dataDir); // disc.cpp
	if (!discInit(disc))
		return 1;
	// the jump table for MEM1, then the translated code (each function checked against the DOL's code)
	PPCRecompiler_allocateRange(kMem1, kMem1Size);
	uint32 counts[4] = {};
	std::string why;
	// hardware register accesses from translated code: straight to the device emulation (format 6),
	// no fault (SSA_HW_DIRECT=0: through the fault path, as before)
	if (!getenv("SSA_HW_DIRECT") || atoi(getenv("SSA_HW_DIRECT")))
	{
		g_cremaAotHwRead = mmioHwRead;
		g_cremaAotHwWrite = mmioHwWrite;
	}
	// supervisor instructions in translated code run in place (format 7, aotSys; SSA_AOT_SYS=0: they
	// leave to the interpreter, as before)
	if (!getenv("SSA_AOT_SYS") || atoi(getenv("SSA_AOT_SYS")))
		g_cremaAotSys = aotSys;
	// GX gather-pipe writes appended by translated code itself (format 8; with the GX thread, no access
	// log or histogram; SSA_AOT_GP=0: through hwWrite, as before)
	if (mmioGatherInline())
	{
		g_cremaAotGp = mmioGatherBuffer();
		g_cremaAotGpFill = mmioGatherFill();
		g_cremaAotGpBurst = mmioGatherBurst;
		fprintf(stderr, "ssa: GX FIFO writes inline in translated code\n");
	}
	if (!CremaAotTestLoad(aotPath, counts, why))
	{
		fprintf(stderr, "ssa: %s can't be used: %s\n", aotPath, why.c_str());
		return 1;
	}
	CremaAotTestDirectCalls(true);
	fprintf(stderr, "ssa: translated code: %u functions active, %u changed, %u invalid; entry %08x\n", counts[0], counts[2], counts[3], entry);
	// (for profilers: where the entry's translated function is - the image's place in memory, against
	// its offset in the file's symbols. The jump table's slot holds its stub: mov edx, entry; mov rax,
	// function; jmp bridge)
	{
		const uint8* stub = (const uint8*)((PPCREC_JUMP_ENTRY*)(void*)ppcRecompilerInstanceData)[entry / 4];
		uint64 fn = 0;
		if (stub && stub[0] == 0xBA && stub[5] == 0x48 && stub[6] == 0xB8)
			memcpy(&fn, stub + 7, 8);
		fprintf(stderr, "ssa: aot_%08x at %p\n", entry, (void*)fn);
		s_aotEntryFn = fn;
	}

	coreinit::g_ssaTimeBase = ssaTimeBase;
	extern void mmioReset(); // mmio.cpp: power-on register values
	mmioReset();
	extern uint64_t (*g_mmioTimeBase)(); // mmio.cpp: counters (AI samples)
	g_mmioTimeBase = ssaTimeBase;
	g_wiiDecRead = decRead;
	g_wiiDecWrite = decWrite;
	// the CPU as the boot program leaves it
	static PPCInterpreter_t cpu{};
	cpu.global = global;
	cpu.LSQE = 1;
	cpu.PSE = 1;
	cpu.gpr[1] = 0x816FFFF0;
	cpu.sprExtended.msr = 0x00002032; // FP, IR, DR, RI (Dolphin's SetupMSR)
	cpu.instructionPointer = entry;
	s_cpu = &cpu;
	PPCInterpreter_setCurrentInstance(&cpu);

	struct sigaction sa{};
	sa.sa_sigaction = onFault;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	struct sigaction trap{};
	trap.sa_sigaction = onTrap;
	trap.sa_flags = SA_SIGINFO;
	sigaction(SIGTRAP, &trap, nullptr);
#ifdef SSA_PS4
	std::thread([] {
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
			onSecond(0);
		}
	}).detach();
#else
	struct sigaction tick{};
	tick.sa_handler = onSecond;
	tick.sa_flags = SA_RESTART;
	sigaction(SIGALRM, &tick, nullptr);
	itimerval timer{};
	timer.it_interval.tv_sec = 1;
	timer.it_value.tv_sec = 1;
	setitimer(ITIMER_REAL, &timer, nullptr);
#endif
	const int limit = getenv("SSA_SECONDS") ? atoi(getenv("SSA_SECONDS")) : 20;
	const uint32 s_watch = getenv("SSA_WATCH") ? (uint32)strtoul(getenv("SSA_WATCH"), nullptr, 16) : 0xFFFFFFFFu;
	int s_watchHits = 0;
	// SSA_INTERP=1: no translated code (the dispatcher sees every instruction: for SSA_WATCH/SSA_TRACE)
	bool s_interpOnly = getenv("SSA_INTERP") != nullptr;
	// SSA_INTERP_AFTER=seconds: translated code until then, the interpreter after (to trace or watch
	// code near a late fault: calls inside translated code don't come back to the dispatcher)
	const int s_interpAfter = getenv("SSA_INTERP_AFTER") ? atoi(getenv("SSA_INTERP_AFTER")) : -1;
	const bool s_iprof = getenv("SSA_IPROF") != nullptr;
	std::map<uint32, uint64> s_iprofCounts;
	// SSA_RUNPROF=1: entries into translated code by address (where runs start: code that returns to
	// the dispatcher often), reported at the end
	const bool s_runProf = getenv("SSA_RUNPROF") != nullptr;
	std::unordered_map<uint32, uint64> s_runCounts;
	if (s_runProf)
	{
		s_sysCounts = new std::unordered_map<uint32, uint64>();
		s_sysProfFrom = getenv("SSA_PROF_FROM") ? atoi(getenv("SSA_PROF_FROM")) : 0;
	}
	auto runProfReport = [&]() {
		if (!s_runProf)
			return;
		std::vector<std::pair<uint64, uint32>> v;
		for (auto& [k, n] : s_runCounts)
			v.push_back({n, k});
		std::sort(v.rbegin(), v.rend());
		fprintf(stderr, "ssa: translated-code entries by address:\n");
		for (size_t i = 0; i < v.size() && i < 40; i++)
			fprintf(stderr, "  %08x: %llu\n", v[i].second, (unsigned long long)v[i].first);
		v.clear();
		for (auto& [k, n] : *s_sysCounts)
			v.push_back({n, k});
		std::sort(v.rbegin(), v.rend());
		fprintf(stderr, "ssa: instructions run in place for translated code (aotSys) by address:\n");
		for (size_t i = 0; i < v.size() && i < 30; i++)
			fprintf(stderr, "  %08x %08x: %llu\n", v[i].second, rd32(v[i].second), (unsigned long long)v[i].first);
	};
	auto iprofReport = [&]() {
		runProfReport();
		if (!s_iprof)
			return;
		std::vector<std::pair<uint64, uint32>> v;
		for (auto& [k, n] : s_iprofCounts)
			v.push_back({n, k});
		std::sort(v.rbegin(), v.rend());
		fprintf(stderr, "ssa: interpreted instructions by block (EE, FP):\n");
		for (size_t i = 0; i < v.size() && i < 40; i++)
			fprintf(stderr, "  %08x EE%u FP%u: %llu\n", v[i].second & ~0xFFu, v[i].second & 1, (v[i].second >> 1) & 1,
				(unsigned long long)v[i].first);
	};
	std::set<uint32> s_trace;
	int s_traceCount = 0;
	if (const char* t = getenv("SSA_TRACE"))
		for (char* p = (char*)t; *p;)
		{
			s_trace.insert((uint32)strtoul(p, &p, 16));
			while (*p == ',')
				p++;
		}
	const bool tracing = !s_trace.empty(); // (the loop's check: a set's empty() showed in PS4 profiles)
	if (const char* w = getenv("SSA_WWATCH"); w && strchr(w, ','))
	{
		char* p = nullptr;
		s_wrangeLo = (uint32)strtoul(w, &p, 16) & ~0xFFFu;
		s_wrangeHi = ((uint32)strtoul(p + 1, nullptr, 16) + s_wrangeLo + 0xFFF) & ~0xFFFu;
		hostProtect(memory_base + s_wrangeLo, s_wrangeHi - s_wrangeLo, false);
		fprintf(stderr, "ssa: counting writes to %08x-%08x\n", s_wrangeLo, s_wrangeHi);
	}
	else if (w)
	{
		s_wwatch = (uint32)strtoul(w, nullptr, 16) & ~3u;
		s_wwatchPage = (uint8*)((uintptr_t)(memory_base + s_wwatch) & ~(hostctx::kPage - 1));
		hostProtect(s_wwatchPage, hostctx::kPage, false);
		fprintf(stderr, "ssa: watching writes to %08x\n", s_wwatch);
	}
	// (with translated code: calls go through the jump table, and the traced functions' slots are
	// unlinked, so a call to one leaves to the dispatcher, which sees it and interprets that function)
	if (!s_trace.empty() && !s_interpOnly)
	{
		CremaAotTestDirectCalls(false);
		for (uint32 a : s_trace)
			if (a >= kMem1 && a < kMem1 + kMem1Size)
				((PPCREC_JUMP_ENTRY*)(void*)ppcRecompilerInstanceData)[a / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}

	// the disc's own apploader, as the boot program runs it: it places the FST and BI2 (and so the
	// arena's top) where the game expects them
	if (runApploader(cpu, disc, entry))
		cpu.instructionPointer = entry;
	// (after the apploader: the game's code in memory) __AI_SRC_INIT natively (mmio.cpp aiSrcInitNative), when the code there is the expected one: its
	// translation switched off, so callers come back here at its entry (SSA_AI_SRC_NATIVE=0: off)
	{
		const char* e = getenv("SSA_AI_SRC_NATIVE");
		if ((!e || atoi(e) != 0) && rd32(kAiSrcInit + 0x68) == 0x801F6C08 && rd32(kAiSrcInit + 0x74) == 0x4182FFF4)
		{
			PPCRecompiler_disableAt(kAiSrcInit);
			s_aiSrcNative = true;
			fprintf(stderr, "ssa: __AI_SRC_INIT at %08x done natively\n", kAiSrcInit);
		}
		else
			fprintf(stderr, "ssa: __AI_SRC_INIT stays translated (%08x %08x there)\n", rd32(kAiSrcInit + 0x68), rd32(kAiSrcInit + 0x74));
	}
	// the AXFX reverb natively, when its code is the expected one (its skip test, a tap load, the 96-sample loop)
	{
		const char* e = getenv("SSA_REVERB_NATIVE");
		if ((!e || atoi(e) != 0) && rd32(kReverbHi + 0x14) == 0x8004010C && rd32(kReverbHi + 0x15C) == 0x7D19E42E && rd32(kReverbHi + 0x504) == 0x281F0060 &&
			rd32(kReverbJump) == 0x48000300)
		{
			PPCRecompiler_disableAt(kReverbHi);
			PPCRecompiler_disableAt(kReverbJump);
			s_reverbNative = true;
			fprintf(stderr, "ssa: the AXFX reverb at %08x runs natively\n", kReverbHi);
		}
		else
			fprintf(stderr, "ssa: the AXFX reverb stays translated\n");
	}
	cpu.gpr[1] = 0x816FFFF0;
	cpu.sprExtended.msr = 0x00002032;
	auto enter = (void (*)(uint64, uint64, uint64, uint64))PPCRecompiler_enterRecompilerCode;
	const auto* table = (PPCREC_JUMP_ENTRY*)(void*)ppcRecompilerInstanceData;
	_mm_setcsr(0x9FC0); // the PS4's float mode: flush-to-zero, denormals-are-zero
	// the devices' polling (see the loop and pollDevices): every s_pollTsc time-stamp ticks
	if (getenv("SSA_POLL_TSC"))
		s_pollTsc = strtoull(getenv("SSA_POLL_TSC"), nullptr, 0);
	bool lastEE = false;
	s_findEntries = getenv("SSA_FIND_ENTRIES") && atoi(getenv("SSA_FIND_ENTRIES"));
	int lastEntryReport = 0;
	if (sigsetjmp(s_stop, 1) == 0)
	{
		s_running = 1;
		while (s_seconds < limit)
		{
			// interrupts, between runs: IOS's queued replies go out, then an external interrupt
			// (PI cause & mask) or the decrementer, when MSR.EE allows. Not after every run: the
			// devices and the interrupt lines are looked at every few microseconds (SSA_POLL_TSC
			// time-stamp ticks, default 32768: ~20 us on the PS4), and at once after a hardware write
			// (g_ssaKick) or when interrupts were just enabled. (Every run cost a clock read and
			// dozens of register lookups: millions a second with the interpreter's single steps.)
			const uint64 tscNow = __rdtsc();
			const bool ee = (cpu.sprExtended.msr & kMsrEE) != 0;
			const bool poll = g_ssaKick || tscNow - s_lastPollTsc >= s_pollTsc || (ee && !lastEE);
			lastEE = ee;
			if (s_findEntries && (int)s_seconds >= lastEntryReport + 10)
			{
				lastEntryReport = (int)s_seconds;
				reportEntries();
			}
			if (poll)
				pollDevices(tscNow);
			if (poll && ee)
			{
				if (wiiInterruptPending())
				{
					// (by cause bit, for the report at the end)
					const uint32 causes = wiiPendingCauses();
					for (int b = 0; b < 32; b++)
						if (causes & (1u << b))
							s_causeCounts[b]++;
					takeException(cpu, 0x500, s_externalTaken);
				}
				else if (decrementerDue())
					takeException(cpu, 0x900, s_decTaken);
			}
			// FP off in OS-run code: the OS's FPU switch, done here (see fpuSwitch)
			if (fpUnavailable(cpu))
				fpuSwitch(cpu);
			if (s_interpAfter >= 0 && !s_interpOnly && s_seconds >= s_interpAfter)
			{
				s_interpOnly = true;
				fprintf(stderr, "ssa: interpreter only from now (%d s)\n", (int)s_seconds);
			}
			const uint32 ip = cpu.instructionPointer;
			s_lastIp = ip;
			g_ssaHwIp = ip;
			// (SSA_REVERB_CHECK=n: the first n calls also run as the game's code in the interpreter, from the
			// same state, and every byte they touch is compared; the game's result stays)
			// (SSA_REVERB_CHECK_FROM=seconds: the checks start then)
			static int s_reverbChecks = getenv("SSA_REVERB_CHECK") ? atoi(getenv("SSA_REVERB_CHECK")) : 0;
			static const int s_reverbCheckFrom = getenv("SSA_REVERB_CHECK_FROM") ? atoi(getenv("SSA_REVERB_CHECK_FROM")) : 0;
			if ((ip == kReverbHi || ip == kReverbJump) && s_reverbNative && s_reverbChecks > 0 && (int)s_seconds >= s_reverbCheckFrom)
			{
				s_reverbChecks--;
				const auto regions = axfxReverbHiRegions(cpu.gpr[3], cpu.gpr[4]);
				std::vector<std::vector<uint8>> before, native;
				for (const auto& [a, n] : regions)
					before.emplace_back(memory_base + a, memory_base + a + n);
				const bool ran = axfxReverbHiNative(cpu.gpr[3], cpu.gpr[4]);
				for (const auto& [a, n] : regions)
					native.emplace_back(memory_base + a, memory_base + a + n);
				for (size_t i = 0; i < regions.size(); i++)
					memcpy(memory_base + regions[i].first, before[i].data(), regions[i].second);
				PPCInterpreter_t saved = cpu;
				constexpr uint32 kBack = 0x80000004; // (a return address nothing else uses: the low vectors)
				cpu.spr.LR = kBack;
				cpu.instructionPointer = kReverbHi;
				uint64 steps = 0;
				while (cpu.instructionPointer != kBack && steps++ < 5000000)
					PPCInterpreterWii_executeInstruction(&cpu);
				size_t differ = 0, firstAt = 0;
				uint32 firstRegion = 0;
				for (size_t i = 0; i < regions.size(); i++)
					for (uint32 k = 0; k < regions[i].second; k++)
						if (memory_base[regions[i].first + k] != native[i][k] && !differ++)
							firstRegion = (uint32)i, firstAt = k;
				// (how much of what it touched isn't zero: a check on silence proves little)
				size_t nonzero = 0;
				for (size_t i = 0; i < regions.size(); i++)
					for (uint32 k = 0; k < regions[i].second; k++)
						nonzero += native[i][k] != 0;
				fprintf(stderr, "ssa: reverb check: native %s, game's code %llu instructions; %zu regions (%zu bytes not zero), %zu bytes differ%s\n",
					ran ? "ran" : "declined", (unsigned long long)steps, regions.size(), nonzero, differ, differ ? "" : " (identical)");
				if (differ)
					fprintf(stderr, "ssa: reverb check: first difference in region %u (%08x +%zx): game %02x native %02x\n", firstRegion, regions[firstRegion].first,
						firstAt, memory_base[regions[firstRegion].first + firstAt], native[firstRegion][firstAt]);
				const PPCInterpreter_t after = cpu;
				cpu = saved;
				cpu.instructionPointer = saved.spr.LR;
				(void)after;
				continue;
			}
			if ((ip == kReverbHi || ip == kReverbJump) && s_reverbNative && axfxReverbHiNative(cpu.gpr[3], cpu.gpr[4]))
			{
				if (!s_reverbNativeCalls++)
					fprintf(stderr, "ssa: the AXFX reverb's first native call (from %08x)\n", cpu.spr.LR);
				cpu.instructionPointer = cpu.spr.LR;
				continue;
			}
			if (ip == kAiSrcInit && s_aiSrcNative)
			{
				aiSrcInitNative();
				fprintf(stderr, "ssa: __AI_SRC_INIT called from %08x: done natively\n", cpu.spr.LR);
				cpu.instructionPointer = cpu.spr.LR;
				continue;
			}
			// SSA_WATCH=address: the registers when execution gets there (the first 4 times)
			if (ip == s_watch && s_watchHits < 4)
			{
				s_watchHits++;
				fprintf(stderr, "ssa: watch %08x: lr %08x ctr %08x msr %08x srr0 %08x srr1 %08x\n", ip, cpu.spr.LR, cpu.spr.CTR,
					cpu.sprExtended.msr, cpu.sprExtended.srr0, cpu.sprExtended.srr1);
				for (int i = 0; i < 32; i += 8)
					fprintf(stderr, "  r%-2d %08x %08x %08x %08x %08x %08x %08x %08x\n", i, cpu.gpr[i], cpu.gpr[i + 1], cpu.gpr[i + 2],
						cpu.gpr[i + 3], cpu.gpr[i + 4], cpu.gpr[i + 5], cpu.gpr[i + 6], cpu.gpr[i + 7]);
				// r3-r6 pointing at text in MEM1: the text (OSFatal/OSPanic messages, file names)
				for (int r = 3; r <= 6; r++)
				{
					const uint32 p = cpu.gpr[r];
					if (p >= 0x80000000 && p < 0x81800000 && memory_base[p] >= 0x20 && memory_base[p] < 0x7F)
						fprintf(stderr, "  r%d -> \"%.200s\"\n", r, (const char*)(memory_base + p));
				}
				// SSA_SNAP=prefix: MEM1 and MEM2 to <prefix>-<hit>.mem1/.mem2 (to diff two runs)
				if (const char* snap = getenv("SSA_SNAP"))
				{
					char path[512];
					snprintf(path, sizeof(path), "%s-%d.mem1", snap, s_watchHits);
					if (FILE* f = fopen(path, "wb"))
					{
						fwrite(memory_base + 0x80000000u, 1, kMem1Size, f);
						fclose(f);
					}
					snprintf(path, sizeof(path), "%s-%d.mem2", snap, s_watchHits);
					if (FILE* f = fopen(path, "wb"))
					{
						fwrite(memory_base + 0x90000000u, 1, 0x04000000, f);
						fclose(f);
					}
					fprintf(stderr, "  snapshot %s-%d written\n", snap, s_watchHits);
				}
				// SSA_DUMP=address: 16 words there too
				if (const char* d = getenv("SSA_DUMP"))
					for (char* p = (char*)d; *p;)
					{
						// (rN+off: relative to a register)
						uint32 at;
						if (*p == 'r')
						{
							const int reg = (int)strtoul(p + 1, &p, 10) & 31;
							at = cpu.gpr[reg] + (*p == '+' ? (uint32)strtoul(p + 1, &p, 16) : 0);
						}
						else
							at = (uint32)strtoul(p, &p, 16);
						fprintf(stderr, "  %08x:", at);
						for (int i = 0; i < 16; i++)
							fprintf(stderr, " %08x", rd32(at + i * 4));
						fprintf(stderr, "\n");
						// words that point to text: the text
						for (int i = 0; i < 16; i++)
						{
							const uint32 q = rd32(at + i * 4);
							if (((q >= 0x80000000 && q < 0x81800000) || (q >= 0x90000000 && q < 0x94000000)) &&
								memory_base[q] >= 0x20 && memory_base[q] < 0x7F && memory_base[q + 1] >= 0x20 && memory_base[q + 1] < 0x7F)
								fprintf(stderr, "    +%02x -> \"%.120s\"\n", i * 4, (const char*)(memory_base + q));
						}
						while (*p == ',')
							p++;
					}
			}
			// SSA_TRACE: entries to the listed functions (with SSA_INTERP=1 every call is seen)
			if (tracing && s_trace.count(ip) && s_traceCount < 2000)
			{
				s_traceCount++;
				fprintf(stderr, "trace: %08x from lr %08x  r3 %08x r4 %08x r5 %08x\n", ip, cpu.spr.LR, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5]);
			}
			const PPCREC_JUMP_ENTRY slot = (!s_interpOnly && ip >= kMem1 && ip < kMem1 + kMem1Size) ? table[ip / 4] : nullptr;
			if (slot && slot != PPCRecompiler_leaveRecompilerCode_unvisited && slot != PPCRecompiler_leaveRecompilerCode_visited)
			{
				static const int runProfFrom = getenv("SSA_PROF_FROM") ? atoi(getenv("SSA_PROF_FROM")) : 0;
				if (s_runProf && (int)s_seconds >= runProfFrom)
					s_runCounts[ip]++;
				cpu.remainingCycles = 200000;
				const uint64 t0 = __rdtsc();
				enter(0, 0, (uint64)&cpu, (uint64)slot);
				g_ssaTicksTranslated = g_ssaTicksTranslated + (__rdtsc() - t0);
				s_translatedRuns = s_translatedRuns + 1;
				// left where it was entered (a leave at its first instruction): that one interpreted
				if (cpu.instructionPointer != ip)
					continue;
			}
			cpu.remainingCycles = 1000;
			// SSA_IPROF=1: where interpreted instructions go (by 256-byte block, with the MSR's EE/FP);
			// SSA_PROF_FROM=seconds: both profiles from then on
			static const int profFrom = getenv("SSA_PROF_FROM") ? atoi(getenv("SSA_PROF_FROM")) : 0;
			if (s_iprof && (int)s_seconds >= profFrom)
				s_iprofCounts[((ip & ~0xFFu) << 0) | ((cpu.sprExtended.msr >> 13) & 1) << 1 | ((cpu.sprExtended.msr >> 15) & 1)]++;
			// a short batch: the next instructions too while they have no translation, the MSR stays
			// the same (no exception, rfi or mtmsr) and no hardware write happened; the loop's checks
			// (interrupts, devices, watches) come between batches. SSA_INTERP_BATCH=1: one at a time.
			static const int batch = getenv("SSA_INTERP_BATCH") ? std::max(1, atoi(getenv("SSA_INTERP_BATCH"))) : 32;
			const uint32 msr0 = cpu.sprExtended.msr;
			const bool single = tracing || s_iprof;
			// SSA_FIND_ENTRIES=1: functions that run interpreted because the entry list missed them
			// (called through pointers): a call's target with no translation, here from translated
			// code (the instruction before LR calls this address) and below from interpreted code
			if (s_findEntries && ip >= kMem1 && ip < kMem1 + kMem1Size && cpu.spr.LR >= kMem1 + 4 && cpu.spr.LR < kMem1 + kMem1Size)
			{
				const uint32 w = rd32(cpu.spr.LR - 4);
				const uint32 at = cpu.spr.LR - 4;
				const bool bl = (w >> 26) == 18 && (w & 1) && ((((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000) + ((w & 2) ? 0 : at)) == ip;
				const bool bctrl = (w >> 26) == 19 && (w & 1) && (((w >> 1) & 0x3FF) == 528 || ((w >> 1) & 0x3FF) == 16);
				if (bl || bctrl)
					noteEntry(ip);
				// (and wherever interpretation starts without a translation: a switch's jump-table
				// target past the end the analysis gave its function, a return into such code)
				noteEntry(ip | 1);
			}
			const uint64 t1 = __rdtsc();
			int done = 0;
			for (;;)
			{
				const uint32 lr0 = cpu.spr.LR, ip0 = cpu.instructionPointer;
				PPCInterpreterWii_executeInstruction(&cpu);
				done++;
				// (a call, or a bctr: a switch statement's jump)
				const bool viaCtr = s_findEntries && ip0 >= kMem1 && ip0 < kMem1 + kMem1Size && rd32(ip0) == 0x4E800420;
				if (s_findEntries && ((cpu.spr.LR != lr0 && cpu.spr.LR == ip0 + 4 && cpu.instructionPointer != ip0 + 4) || viaCtr))
				{
					const uint32 t = cpu.instructionPointer;
					if (t >= kMem1 && t < kMem1 + kMem1Size)
					{
						const PPCREC_JUMP_ENTRY s = table[t / 4];
						if (!s || s == PPCRecompiler_leaveRecompilerCode_unvisited || s == PPCRecompiler_leaveRecompilerCode_visited)
							noteEntry(t);
					}
				}
				if (done >= batch || single || g_ssaKick || cpu.sprExtended.msr != msr0)
					break;
				const uint32 next = cpu.instructionPointer;
				if (next == kAiSrcInit || next == kReverbHi || next == kReverbJump || next == s_watch)
					break;
				if (!s_interpOnly && next >= kMem1 && next < kMem1 + kMem1Size)
				{
					const PPCREC_JUMP_ENTRY s = table[next / 4];
					if (s && s != PPCRecompiler_leaveRecompilerCode_unvisited && s != PPCRecompiler_leaveRecompilerCode_visited)
						break;
				}
				cpu.remainingCycles = 1000;
			}
			g_ssaTicksInterp = g_ssaTicksInterp + (__rdtsc() - t1);
			s_interpreted = s_interpreted + done;
		}
		s_running = 0;
		fprintf(stderr, "ssa: stopped after %d s at %08x (no fault)\n", limit, cpu.instructionPointer);
		aotDump();
		fprintf(stderr, "ssa: external interrupts %llu, decrementer %llu, FPU switches %llu; by PI cause:",
			(unsigned long long)s_externalTaken, (unsigned long long)s_decTaken, (unsigned long long)s_fpuSwitches);
		for (int b = 0; b < 32; b++)
			if (s_causeCounts[b])
				fprintf(stderr, " %x:%llu", 1u << b, (unsigned long long)s_causeCounts[b]);
		fprintf(stderr, "\n");
		fprintf(stderr, "ssa: audio: %llu AX frames, %llu DMA buffers played\n", (unsigned long long)dspAxFrames(),
			(unsigned long long)dspDmaBuffers());
		dumpThreads(cpu);
		wrangeReport();
		iprofReport();
		gxReport();
		return 0;
	}
	const uint32 g = s_faultGuest;
	fprintf(stderr, "ssa: signal %d: %s access at guest %08x (%s), host rip %llx; guest ip %08x lr %08x r1 %08x\n", s_faultSignal,
		g == 0xFFFFFFFFu ? "host" : "guest", g, g == 0xFFFFFFFFu ? "-" : blockName(g), (unsigned long long)s_faultRip, s_lastIp,
		cpu.spr.LR, cpu.gpr[1]);
	fprintf(stderr, "ssa: %llu translated runs, %llu instructions interpreted\n", (unsigned long long)s_translatedRuns,
		(unsigned long long)s_interpreted);
	// the host side: the address, registers, the code around rip (a fault reading it ends the app,
	// after these lines)
	{
		static const char* kNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
		fprintf(stderr, "ssa: host fault address %llx; memory_base %p, recompiler data %p\n  host regs:", (unsigned long long)s_faultAddr,
			memory_base, (void*)ppcRecompilerInstanceData);
		for (int i = 0; i < 16; i++)
			fprintf(stderr, " %s=%llx", kNames[i], (unsigned long long)s_faultRegs[i]);
		fprintf(stderr, "\n");
		if (s_faultAddr != s_faultRip)
		{
			const uint8* code = (const uint8*)s_faultRip;
			fprintf(stderr, "  code at rip-16:");
			for (int i = -16; i < 16; i++)
				fprintf(stderr, "%s%02x", i == 0 ? " [" : " ", code[i]);
			fprintf(stderr, "\n");
		}
		else
			fprintf(stderr, "  (the fault is at rip itself: executing there)\n");
	}
	ps4_crash_jit_info(2, s_faultRip, 1); // (the translated function the host code is in)
	// SSA_SNAP: memory at the fault too (<prefix>-fault.mem1/.mem2)
	if (const char* snap = getenv("SSA_SNAP"))
	{
		char path[512];
		snprintf(path, sizeof(path), "%s-fault.mem1", snap);
		if (FILE* f = fopen(path, "wb"))
		{
			fwrite(memory_base + kMem1, 1, kMem1Size, f);
			fclose(f);
		}
		snprintf(path, sizeof(path), "%s-fault.mem2", snap);
		if (FILE* f = fopen(path, "wb"))
		{
			fwrite(memory_base + kMem2, 1, kMem2Size, f);
			fclose(f);
		}
	}
	wrangeReport();
	// the guest's call stack from r1 (exact when the fault was in interpreted code)
	fprintf(stderr, "ssa: guest stack:");
	for (uint32 frame = cpu.gpr[1], i = 0; frame && i < 16; i++)
	{
		const uint32 next = rd32(frame);
		if (!next || next <= frame)
			break;
		fprintf(stderr, " %08x", rd32(next + 4));
		frame = next;
	}
	fprintf(stderr, "\n  regs:");
	for (int i = 0; i < 32; i++)
		fprintf(stderr, " r%d=%08x", i, cpu.gpr[i]);
	fprintf(stderr, "\n");
#ifndef SSA_PS4
	void* frames[16];
	const int n = backtrace(frames, 16);
	backtrace_symbols_fd(frames, n, 2);
#endif
	return 2;
}
