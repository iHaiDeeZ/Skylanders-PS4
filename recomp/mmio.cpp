// Skylanders runtime: Wii hardware registers (0xCC000000-0xCDFFFFFF), reached through faults.
//
// The register areas are left unmapped in guest memory. A load or store there faults; onFault
// (ssa_run.cpp) hands the context to mmioEmulate, which decodes the faulting x86 instruction (the
// forms clang emits for the translated code's and the interpreter's memory accesses: mov, movbe,
// movzx/movsx, byte/word/dword, register or immediate), calls the device layer with the guest address
// and value, writes a loaded value into the destination register and steps past the instruction.
//
// Guest memory is big-endian: a plain mov sees the register's bytes swapped (the code swaps them
// back), movbe sees the guest value itself.
//
// This is for bring-up: a fault per access is far too slow for the GX FIFO's per-vertex writes,
// which get direct appends in the recompiler later (PLAN.md P3).
//
// The device layer, v1: every register is a stored word (what was written reads back), and the first
// access to each register is logged (SSA_MMIO_LOG=1: every access). Devices get real behaviour as the
// boot needs it (mmioDeviceRead / mmioDeviceWrite).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ucontext.h>
#include "host_ctx.h"
#include <unordered_map>
#include <unordered_set>

namespace
{
struct Access
{
	bool store = false;
	int size = 4;          // bytes
	bool movbe = false;    // the value as the guest sees it (no swap)
	bool zeroExtend = false, signExtend = false;
	int destSize = 4;      // for movzx/movsx: the destination register's size
	int reg = -1;          // x86 register number (0-15)
	bool high8 = false;    // AH, CH, DH, BH
	bool immediate = false;
	uint64_t imm = 0;
	int length = 0;
};

// the faulting instruction's form; false if it isn't one handled here
bool decode(const uint8_t* p, Access& a)
{
	const uint8_t* const start = p;
	bool op16 = false;
	int rex = 0;
	for (;;)
	{
		const uint8_t b = *p;
		if (b == 0x66) { op16 = true; p++; continue; }
		if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) { p++; continue; }
		break;
	}
	if ((*p & 0xF0) == 0x40)
		rex = *p++;
	const bool rexW = rex & 8, rexR = rex & 4;
	const int wordSize = rexW ? 8 : (op16 ? 2 : 4);
	int immSize = 0;
	uint8_t op = *p++;
	if (op == 0x8B) { a.size = wordSize; }
	else if (op == 0x89) { a.store = true; a.size = wordSize; }
	else if (op == 0x8A) { a.size = 1; }
	else if (op == 0x88) { a.store = true; a.size = 1; }
	else if (op == 0xC7) { a.store = true; a.size = wordSize; a.immediate = true; immSize = op16 ? 2 : 4; }
	else if (op == 0xC6) { a.store = true; a.size = 1; a.immediate = true; immSize = 1; }
	else if (op == 0x0F)
	{
		const uint8_t op2 = *p++;
		if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF)
		{
			a.size = (op2 & 1) ? 2 : 1;
			a.zeroExtend = op2 < 0xBE;
			a.signExtend = !a.zeroExtend;
			a.destSize = rexW ? 8 : (op16 ? 2 : 4);
		}
		else if (op2 == 0x38 && (p[0] == 0xF0 || p[0] == 0xF1))
		{
			a.store = *p++ == 0xF1;
			a.movbe = true;
			a.size = wordSize;
		}
		else
			return false;
	}
	else
		return false;
	if (a.size == 8)
		return false; // (no 64-bit hardware registers)
	if (!a.zeroExtend && !a.signExtend)
		a.destSize = a.size;
	const uint8_t modrm = *p++;
	const int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
	if (mod == 3)
		return false;
	if (rm == 4)
	{
		const uint8_t sib = *p++;
		if (mod == 0 && (sib & 7) == 5)
			p += 4;
	}
	if (mod == 1)
		p += 1;
	else if (mod == 2 || (mod == 0 && rm == 5))
		p += 4;
	if (a.immediate)
	{
		if (reg != 0)
			return false;
		a.imm = 0;
		for (int i = 0; i < immSize; i++)
			a.imm |= (uint64_t)p[i] << (8 * i);
		p += immSize;
	}
	else
	{
		a.reg = reg | (rexR ? 8 : 0);
		a.high8 = a.size == 1 && !a.zeroExtend && !a.signExtend && rex == 0 && reg >= 4;
		if (a.high8)
			a.reg = reg - 4;
	}
	a.length = (int)(p - start);
	return true;
}



uint32_t swapSize(uint32_t v, int size)
{
	if (size == 4) return __builtin_bswap32(v);
	if (size == 2) return __builtin_bswap16((uint16_t)v);
	return v & 0xFF;
}

const char* blockOf(uint32_t a)
{
	switch (a & 0xFFFFF000)
	{
	case 0xCC000000: return "CP";
	case 0xCC001000: return "PE";
	case 0xCC002000: return "VI";
	case 0xCC003000: return "PI";
	case 0xCC004000: return "MI";
	case 0xCC005000: return "DSP";
	case 0xCC008000: return "GX FIFO";
	case 0xCD000000: return "Hollywood";
	case 0xCD800000: return "Hollywood";
	case 0xCD006000: return (a & 0xFFF) < 0x400 ? "DI" : (a & 0xFFF) < 0x800 ? "SI" : (a & 0xFFF) < 0xC00 ? "EXI" : "AI";
	case 0xCD806000: return (a & 0xFFF) < 0x400 ? "DI" : (a & 0xFFF) < 0x800 ? "SI" : (a & 0xFFF) < 0xC00 ? "EXI" : "AI";
	default: return "?";
	}
}

// the register words, by address (guest values): flat arrays for 0xCC00xxxx and 0xCD00xxxx (0xCD80xxxx
// is the same bank: canonical), anything else in a map. (An unordered_map for all of them cost ~13%
// of the PS4's main thread in v00.08: piCause alone looks at ~15 registers.)
struct RegFile
{
	uint32_t cc[0x4000] = {}, cd[0x4000] = {};
	std::unordered_map<uint32_t, uint32_t> other;
	uint32_t& operator[](uint32_t a)
	{
		const uint32_t hi = a & 0xFF7F0000u;
		if (hi == 0xCC000000u)
			return cc[(a & 0xFFFF) >> 2];
		if (hi == 0xCD000000u)
			return cd[(a & 0xFFFF) >> 2];
		return other[a];
	}
	uint32_t get(uint32_t a)
	{
		const uint32_t hi = a & 0xFF7F0000u;
		if (hi == 0xCC000000u)
			return cc[(a & 0xFFFF) >> 2];
		if (hi == 0xCD000000u)
			return cd[(a & 0xFFFF) >> 2];
		auto it = other.find(a);
		return it == other.end() ? 0 : it->second;
	}
	// every word that isn't 0, with its (canonical) address
	template <class F> void forEach(F f)
	{
		for (uint32_t i = 0; i < 0x4000; i++)
			if (cc[i])
				f(0xCC000000u | (i << 2), cc[i]);
		for (uint32_t i = 0; i < 0x4000; i++)
			if (cd[i])
				f(0xCD000000u | (i << 2), cd[i]);
		for (auto& [a, v] : other)
			f(a, v);
	}
};
RegFile s_regs;
std::unordered_set<uint64_t> s_seen;
bool s_logAll = false;
uint64_t s_accesses = 0;

void logAccess(bool store, uint32_t addr, int size, uint32_t value, uint32_t guestIp)
{
	// (each register's first load and first store are logged: a bit per word for the 0xCC/0xCD
	// blocks, a set for the rest)
	if (!s_logAll)
	{
		const uint32_t hi = addr & 0xFF7F0000u;
		if (hi == 0xCC000000u || hi == 0xCD000000u)
		{
			static uint64_t seen[2][2][0x4000 / 64];
			const uint32_t word = (addr & 0xFFFF) >> 2;
			uint64_t& bits = seen[hi == 0xCD000000u][store][word >> 6];
			const uint64_t bit = 1ull << (word & 63);
			if (bits & bit)
				return;
			bits |= bit;
		}
		else if (!s_seen.insert((uint64_t)addr << 1 | (store ? 1 : 0)).second)
			return;
	}
	char line[128];
	const int n = snprintf(line, sizeof(line), "mmio: %s%d %08x %s %0*x (%s) near guest %08x\n", store ? "W" : "R", size * 8, addr,
		store ? "<-" : "->", size * 2, value, blockOf(addr), guestIp);
	write(2, line, n);
}
} // namespace

// the guest address a register is stored under (the 0xCD8xxxxx Hollywood mirror folded onto 0xCDxxxxxx)
static uint32_t canonical(uint32_t addr)
{
	if ((addr & 0xFF800000) == 0xCD800000)
		return addr & ~0x00800000u;
	return addr;
}

// ---- DSP (0xCC005000): the mailboxes and the audio DMA are dsp.cpp's; the control/status register
// (CSR, the low half of 0xCC005008) is here, with dsp.cpp told of its changes. ARAM DMA
// (0x5020-0x5028; the Wii has no ARAM) completes at once with ARINT. ----
bool dspOwns(uint32_t a); // dsp.cpp
uint32_t dspRead(uint32_t a, int size);
void dspWrite(uint32_t a, uint32_t value, int size);
uint32_t dspControlWritten(uint32_t old, uint32_t csr);
uint32_t dspControlRead(uint32_t csr);

// CSR status bits set by the DSP side (DSPINT bit 7, AIDINT bit 3)
void mmioCsrSet(uint32_t bits)
{
	s_regs[0xCC005008] |= bits;
}

// ---- the GX FIFO and the command processor (CP, 16-bit registers at 0xCC000000), after Dolphin's
// CommandProcessor/GPFifo. The write-gather pipe (0xCC008000) collects bytes into 32-byte bursts that
// go to memory at PI's FIFO write pointer (0xCC003014, from base 0xCC00300C to end 0xCC003010, then
// back to base); in linked mode the CP's write pointer follows it. The GPU takes what is between its
// read and write pointers at once: the read pointer follows the write pointer, except that a breakpoint
// (enabled by CP control bit 1, at 0x3C/0x3E) stops it there. The GX decoder (gx.cpp) gets the
// 32-byte blocks the read pointer passes, from the FIFO in memory: what the GPU reads, when it reads
// it. (Games may build a frame's commands in a CPU FIFO the GPU isn't attached to and hand it over
// later; the vertex data those commands use is filled in meanwhile. Decoding the gather pipe's
// bursts as they were written read that data too early: the level's buildings came out as spikes.) Reaching it sets status bit 4, and CP
// control bit 5 makes that the CP interrupt (PI cause 0x800). GX's breakpoint callback runs from it:
// the game sets the breakpoint at the write pointer after a frame and frees the frame's slot of its
// command ring there (the main loop waits for that).
extern uint8_t* memory_base;
void gxFifoData(const uint8_t* data, size_t size); // gx.cpp
void gxSync();                                     // gx.cpp
bool gxThreaded();                                 // gx.cpp: the GX thread runs (SSA_GX_THREAD=1)
uint64_t gxFifoPosition();                         // gx.cpp: a marker for the bytes so far
bool gxFifoReached(uint64_t position);             // gx.cpp: the GX thread has decoded that far
void gxPeEvents(uint32_t* done, uint32_t* tokenInts, uint32_t* tokenSeq, uint32_t* token);
static bool s_gxSyncPending = false;               // a draw-done or token went into the FIFO
namespace
{
constexpr uint32_t kCp = 0xCC000000;
uint8_t s_gather[64];
uint32_t s_gatherFill = 0; // (uint32_t: translated code's inline writes use it too, patch-aot-gp.py)
uint64_t s_gatherBursts = 0;
// the read pointer stopped at the breakpoint; with the GX thread, reached once the bytes before it are
// decoded (the marker)
bool s_cpStopped = false;
uint64_t s_cpBreakMarker = 0;
bool cpAtBreakpoint()
{
	return s_cpStopped && gxFifoReached(s_cpBreakMarker);
}

uint32_t cp16(uint32_t off)
{
	const uint32_t w = s_regs[kCp + (off & ~3u)];
	return (off & 2) ? (w & 0xFFFF) : (w >> 16);
}
void setCp16(uint32_t off, uint32_t v)
{
	uint32_t& w = s_regs[kCp + (off & ~3u)];
	w = (off & 2) ? ((w & 0xFFFF0000u) | (v & 0xFFFF)) : ((w & 0xFFFFu) | (v << 16));
}
uint32_t cp32(uint32_t off) // (a low/high pair)
{
	return (cp16(off) | (cp16(off + 2) << 16)) & 0x3FFFFFE0u;
}
void setCp32(uint32_t off, uint32_t v)
{
	setCp16(off, v & 0xFFFF);
	setCp16(off + 2, v >> 16);
}

// the GPU's side: read pointer = write pointer unless a breakpoint stops it (control bit 1)
void cpUpdate()
{
	const uint32_t ctrl = cp16(0x02), wptr = cp32(0x34), bp = cp32(0x3C);
	uint32_t rptr = cp32(0x38);
	if (!(ctrl & 2))
		s_cpStopped = false;
	if (!s_cpStopped)
	{
		if ((ctrl & 2) && rptr == bp)
		{
			s_cpStopped = true;
			s_cpBreakMarker = gxFifoPosition();
		}
		else if (ctrl & 1) // GP read enable
		{
			// (to the write pointer, stopping at an enabled breakpoint on the way)
			const uint32_t base = cp32(0x20), end = cp32(0x24);
			for (int guard = 0; rptr != wptr && guard < 0x100000; guard++)
			{
				// (in runs: to the write pointer, the FIFO's end or an enabled breakpoint, each one call)
				uint32_t stop = wptr > rptr ? wptr : end + 32;
				if (stop > end + 32)
					stop = end + 32;
				if ((ctrl & 2) && bp > rptr && bp < stop)
					stop = bp;
				if (stop <= rptr)
					stop = rptr + 32;
				const uint32_t n = stop - rptr;
				if (rptr + n <= 0x01800000 || (rptr >= 0x10000000 && rptr + n <= 0x14000000))
					gxFifoData(memory_base + 0x80000000u + rptr, n);
				rptr = stop;
				if (rptr > end)
					rptr = base;
				if ((ctrl & 2) && rptr == bp)
				{
					s_cpStopped = true;
					s_cpBreakMarker = gxFifoPosition();
					break;
				}
			}
		}
	}
	setCp32(0x38, rptr);
	const uint32_t base = cp32(0x20), end = cp32(0x24);
	setCp32(0x30, wptr >= rptr ? wptr - rptr : (end + 32 - base) - (rptr - wptr)); // RW distance
}

// status: bit 4 breakpoint, bit 3 command idle, bit 2 read idle (bits 0/1: watermarks, never)
uint32_t cpStatus()
{
	return (cpAtBreakpoint() ? 0x10 : 0) | 0x0C;
}

bool cpInterruptLine()
{
	return cpAtBreakpoint() && (cp16(0x02) & 0x20);
}

// with the GX thread: the pixel engine's draw-done and tokens, set as the thread decodes them (without
// it, the gather pipe's writes set them at once: mmioDeviceWrite)
void gxApplyPeEvents()
{
	if (!gxThreaded())
		return;
	static uint32_t lastDone = 0, lastInts = 0, lastSeq = 0;
	uint32_t done, ints, seq, token;
	gxPeEvents(&done, &ints, &seq, &token);
	uint32_t& peWord = s_regs[0xCC001008];
	if (done != lastDone)
	{
		peWord |= 1u << 3;
		lastDone = done;
	}
	if (seq != lastSeq)
	{
		s_regs[0xCC00100C] = (s_regs[0xCC00100C] & 0xFFFF0000u) | (token & 0xFFFF);
		lastSeq = seq;
	}
	if (ints != lastInts)
	{
		peWord |= 1u << 2;
		lastInts = ints;
	}
}

// 32-byte bursts out of the gather pipe (bytes: a multiple of 32), into the FIFO in memory, then the
// command processor's pointers once for all of them
void gatherBursts(const uint8_t* burst, uint32_t bytes)
{
	s_gatherBursts += bytes / 32;
	uint32_t& piWptr = s_regs[0xCC003014];
	const uint32_t base = s_regs[0xCC00300C] & 0x1FFFFFE0u, end = s_regs[0xCC003010] & 0x1FFFFFE0u;
	uint32_t w = piWptr & 0x1FFFFFE0u;
	if (base && end > base)
	{
		for (uint32_t off = 0; off < bytes; off += 32)
		{
			if (w < 0x01800000 || (w >= 0x10000000 && w < 0x14000000))
				memcpy(memory_base + 0x80000000u + w, burst + off, 32);
			w += 32;
			if (w > end)
				w = base;
		}
		piWptr = w;
	}
	// linked mode (CP control bit 4): the CP's write pointer is the PI's
	if (cp16(0x02) & 0x10)
		setCp32(0x34, w);
	cpUpdate();
	if (s_gxSyncPending)
	{
		// (with SSA_GX_DEFER: the GPU has done everything before a draw-done or token by the time
		// the CPU sees it)
		s_gxSyncPending = false;
		gxSync();
	}
}

// (v00.19) The bursts batched: with the GX thread they collect here and go to the FIFO in memory and to
// the thread together - every 2 KiB, before any other hardware access (the game reads the FIFO's
// pointers, the PE's tokens as they would be) and at every device poll (mmioFlushFifo: ~20 us, nothing
// waits on them longer). Per burst the CP/PI update and the hand-over to the GX thread were ~4% of the
// PS4's game thread (v00.17). SSA_FIFO_BATCH=0: each burst at once.
uint8_t s_batch[2048];
uint32_t s_batchFill = 0;

void flushBursts()
{
	if (!s_batchFill)
		return;
	const uint32_t n = s_batchFill;
	s_batchFill = 0;
	gatherBursts(s_batch, n);
}

void gatherBurst(const uint8_t* burst)
{
	static const bool batch = gxThreaded() && (!getenv("SSA_FIFO_BATCH") || atoi(getenv("SSA_FIFO_BATCH")));
	if (!batch)
	{
		gatherBursts(burst, 32);
		return;
	}
	memcpy(s_batch + s_batchFill, burst, 32);
	s_batchFill += 32;
	if (s_batchFill == sizeof(s_batch))
		flushBursts();
}
} // namespace

void mmioFlushFifo()
{
	flushBursts();
}

// ---- interrupts: PI's cause register (0xCC003000) from the devices' lines, its mask at 0xCC003004 ----
// Causes (Dolphin's ProcessorInterface.h): EXI 0x10, DSP 0x40, IPC 0x4000, reset button 0x10000
// (set: not pressed); RSW/error latch (cleared by writing 1).
bool iosInterruptLine(); // ios.cpp
bool iosOwns(uint32_t a);
uint32_t iosRead(uint32_t a);
void iosWrite(uint32_t a, uint32_t v);
namespace
{
uint32_t s_piLatched = 0;
}
uint32_t aiSampleCount();
namespace
{
uint64_t s_fifoBytes = 0; // bytes written to the GX FIFO

uint32_t piCause()
{
	gxApplyPeEvents();
	uint32_t cause = s_piLatched | 0x10000;
	// DSP: AIDINT/ARINT/DSPINT with their masks (CSR bits 3/4, 5/6, 7/8)
	const uint32_t csr = s_regs[0xCC005008] & 0xFFFF;
	if (((csr >> 3) & (csr >> 4) & 1) || ((csr >> 5) & (csr >> 6) & 1) || ((csr >> 7) & (csr >> 8) & 1))
		cause |= 0x40;
	// EXI: per channel, EXIINT/TCINT/EXTINT with their masks (CPR bits 1/0, 3/2, 11/10)
	for (int c = 0; c < 3; c++)
	{
		const uint32_t cpr = s_regs[0xCD006800 + c * 0x14];
		if (((cpr >> 1) & cpr & 1) || ((cpr >> 3) & (cpr >> 2) & 1) || ((cpr >> 11) & (cpr >> 10) & 1))
			cause |= 0x10;
	}
	if (iosInterruptLine())
		cause |= 0x4000;
	if (cpInterruptLine())
		cause |= 0x800;
	// PE (0xCC00100A, the low half of 0xCC001008): token (status bit 2, enable bit 0) and finish
	// (status bit 3, enable bit 1)
	const uint32_t pe = s_regs[0xCC001008] & 0xFFFF;
	if ((pe >> 2) & pe & 1)
		cause |= 0x200;
	if ((pe >> 3) & (pe >> 1) & 1)
		cause |= 0x400;
	// VI: a display interrupt (DI0-DI3 at 0xCC002030-3C) with its status (bit 31) and enable (bit 28)
	for (uint32_t r = 0xCC002030; r <= 0xCC00203C; r += 4)
		if ((s_regs[r] >> 31) & (s_regs[r] >> 28) & 1)
			cause |= 0x100;
	return cause;
}
}

// VI's display interrupts at their lines: NTSC, 525 lines a frame at 59.94 Hz. A DIn register holds
// its line in bits 25-16 (VCT, counted from 1); crossing that point of the frame sets its status bit
// (bit 31) when it is enabled (bit 28); the OS's handler clears it by writing it back with bit 31 clear.
// registers' power-on values (Dolphin): DI config 1 (the boot ROM descrambler off)
void mmioReadSwitches(); // (below)
void mmioReset()
{
	s_regs[0xCD006024] = 1;
	mmioReadSwitches();
}

void viTick(uint64_t timeBase)
{
	static uint64_t s_last = 0;
	// (SSA_VI_HZ=n, the PC's speed tests: n display interrupts a second instead of 59.94 - a game
	// waiting for them then runs as fast as its code allows, so frames a second measure the code)
	static const uint64_t kFrame = getenv("SSA_VI_HZ") && atoi(getenv("SSA_VI_HZ")) > 0 ? 60750000ull / atoi(getenv("SSA_VI_HZ")) : 1013513; // 60.75 MHz / 59.94 Hz
	if (timeBase <= s_last)
		return;
	for (uint32_t r = 0xCC002030; r <= 0xCC00203C; r += 4)
	{
		uint32_t& di = s_regs[r];
		if (!(di & (1u << 28)))
			continue;
		const uint32_t line = (di >> 16) & 0x3FF;
		const uint64_t at = kFrame * (line ? line - 1 : 0) / 525;
		// crossed since the last tick (in this frame or a later one)?
		const uint64_t lastFrame = s_last / kFrame, nowFrame = timeBase / kFrame;
		const uint64_t lastPoint = lastFrame * kFrame + at, nextPoint = lastPoint > s_last ? lastPoint : lastPoint + kFrame;
		if (nextPoint <= timeBase || nowFrame > lastFrame + 1)
			di |= 1u << 31;
	}
	s_last = timeBase;
}

// ---- AI (0xCD006C00): the audio interface's sample counter (AISCNT, 0xCD006C08) counts at 48 kHz
// while playback is on (AICR bit 0), from the time base; AICR bit 5 (SCRESET) zeroes it ----
extern uint64_t (*g_mmioTimeBase)();
namespace
{
uint64_t s_aiStart = 0;   // the time base when the count last started or was reset
uint32_t s_aiBase = 0;    // the count then
bool s_aiPlaying = false;
}

uint32_t aiSampleCount()
{
	if (!s_aiPlaying || !g_mmioTimeBase)
		return s_aiBase;
	return s_aiBase + (uint32_t)((g_mmioTimeBase() - s_aiStart) * 48000 / 60750000);
}

void aiControlWritten(uint32_t old, uint32_t now)
{
	const uint32_t count = aiSampleCount();
	if (now & (1u << 5)) // SCRESET
	{
		s_aiBase = 0;
		s_aiStart = g_mmioTimeBase ? g_mmioTimeBase() : 0;
	}
	const bool play = now & 1;
	if (play != s_aiPlaying)
	{
		s_aiBase = (now & (1u << 5)) ? 0 : count;
		s_aiStart = g_mmioTimeBase ? g_mmioTimeBase() : 0;
		s_aiPlaying = play;
	}
}

// the SDK's __AI_SRC_INIT (AIInit) done natively: it times one sample-counter step at each rate with
// OSGetTime and retries until the step takes ~28 or ~37 us. Every AI access here is a host fault
// (tens of us on the PS4), so the step never measures in range and the loop spins forever. Its only
// lasting effect: AICR's PSTAT (bit 0) and AFR (bit 1) cleared at the end.
void aiSrcInitNative()
{
	uint32_t& cr = s_regs[0xCD006C00];
	const uint32_t old = cr;
	cr &= ~3u;
	aiControlWritten(old, cr);
}

// a register's current word (the renderer: VI's framebuffer addresses)
uint32_t mmioRegValue(uint32_t a)
{
	return s_regs.get(a);
}

// an external interrupt is waiting (PI cause & mask)
bool wiiInterruptPending()
{
	return (piCause() & s_regs[0xCC003004]) != 0;
}

// the causes behind a pending interrupt (diagnostics)
uint32_t wiiPendingCauses()
{
	return piCause() & s_regs[0xCC003004];
}

uint32_t mmioDeviceRead(uint32_t addr, int size)
{
	const uint32_t a = canonical(addr);
	flushBursts(); // (the batched FIFO bursts first: the game sees the FIFO and the GPU as they would be)
	// (registers with their own logic: whole words)
	auto part = [&](uint32_t word) {
		const int shift = (4 - size - (int)(a & 3)) * 8;
		return size == 4 ? word : (word >> shift) & ((1u << (size * 8)) - 1);
	};
	if (iosOwns(a & ~3u))
		return part(iosRead(a & ~3u));
	if ((a & ~3u) == 0xCC003000)
		return part(piCause());
	if ((a & ~3u) == 0xCD006C08)
		return part(aiSampleCount());
	if (a >= 0xCC001000 && a < 0xCC001100)
		gxApplyPeEvents();
	if ((a & ~3u) == kCp)
		setCp16(0x00, cpStatus());
	if (dspOwns(a))
		return dspRead(a, size);
	if ((a & ~3u) == 0xCC005008)
	{
		uint32_t& csrWord = s_regs[0xCC005008];
		csrWord = (csrWord & 0xFFFF0000u) | dspControlRead(csrWord & 0xFFFF);
	}
	const uint32_t word = s_regs[a & ~3u];
	const int shift = (4 - size - (int)(a & 3)) * 8; // big-endian position in the word
	return size == 4 ? word : (word >> shift) & ((1u << (size * 8)) - 1);
}

// ---- EXI (serial bus: 3 channels at 0xCD006800 + 0x14 * channel; CPR, MAR, LENGTH, CR, DATA) ----
// A transfer runs at once when CR's start bit is written: immediate (1-4 bytes through DATA) or DMA
// (MAR/LENGTH, physical addresses), then the start bit clears and CPR's transfer-complete bit (TCINT)
// is set. Devices: channel 0 device 1 is the RTC/SRAM chip ("IPL" in Dolphin's EXI_DeviceIPL.cpp):
// the first 4 bytes after a select are a command (bit 31: write; bits 30-6: address: 0x800000 RTC,
// 0x800004 SRAM), the bytes after it read or write there. Nothing else is attached (no memory cards):
// reads give zeros.
extern uint8_t* memory_base;
namespace
{
struct ExiIpl
{
	int commandBytes = 0;
	uint32_t command = 0;
	uint32_t offset = 0;
	uint8_t sram[64] = {};
} s_ipl;
uint32_t s_exiSelected[3] = {}; // the CPR's chip select bits the last transfer saw, per channel

uint8_t iplByte(bool write, uint8_t in)
{
	if (s_ipl.commandBytes < 4)
	{
		s_ipl.command = (s_ipl.command << 8) | in;
		if (++s_ipl.commandBytes == 4)
			s_ipl.offset = 0;
		return 0;
	}
	const uint32_t address = (s_ipl.command >> 6) & 0x1FFFFFF;
	uint8_t out = 0;
	if (address == 0x800000)
	{
		// RTC: seconds since 2000-01-01, big-endian
		const uint32_t rtc = (uint32_t)(time(nullptr) - 946684800);
		out = s_ipl.offset < 4 ? (uint8_t)(rtc >> (24 - 8 * s_ipl.offset)) : 0;
	}
	else if (address >= 0x800004 && address < 0x800004 + 16)
	{
		const uint32_t at = (address - 0x800004) * 4 + s_ipl.offset;
		if (at < 64)
		{
			if (write && (s_ipl.command & 0x80000000u))
				s_ipl.sram[at] = in;
			out = s_ipl.sram[at];
		}
	}
	s_ipl.offset++;
	return out;
}

// one byte through the selected device
uint8_t exiByte(int channel, uint32_t cs, bool write, uint8_t in)
{
	if (channel == 0 && cs == 2)
		return iplByte(write, in);
	return 0;
}

void exiTransfer(int channel)
{
	const uint32_t base = 0xCD006800 + channel * 0x14;
	uint32_t& cpr = s_regs[base];
	uint32_t& cr = s_regs[base + 0xC];
	const uint32_t cs = (cpr >> 7) & 7;
	if (cs != s_exiSelected[channel])
	{
		// a new select: the IPL device's command starts over
		if (channel == 0)
			s_ipl.commandBytes = 0;
		s_exiSelected[channel] = cs;
	}
	const bool dma = cr & 2;
	const uint32_t rw = (cr >> 2) & 3; // 0 read, 1 write, 2 read-write
	char line[160];
	if (!dma)
	{
		const int len = ((cr >> 4) & 3) + 1;
		uint32_t& data = s_regs[base + 0x10];
		uint32_t result = 0;
		for (int i = 0; i < len; i++)
		{
			const uint8_t in = (uint8_t)(data >> (24 - 8 * i));
			const uint8_t out = exiByte(channel, cs, rw != 0, in);
			result |= (uint32_t)out << (24 - 8 * i);
		}
		if (rw != 1)
			data = result;
		const int n = snprintf(line, sizeof(line), "exi: ch%d cs%x imm %s %d bytes, data %08x\n", channel, cs, rw == 0 ? "read" : rw == 1 ? "write" : "rw", len,
			data);
		write(2, line, n);
	}
	else
	{
		const uint32_t mar = s_regs[base + 4] & 0x1FFFFFE0, len = s_regs[base + 8] & 0x3FFFFE0;
		uint8_t* mem = memory_base + 0x80000000u + mar;
		for (uint32_t i = 0; i < len; i++)
			mem[i] = rw == 1 ? (exiByte(channel, cs, true, mem[i]), mem[i]) : exiByte(channel, cs, false, 0);
		const int n = snprintf(line, sizeof(line), "exi: ch%d cs%x dma %s %u bytes at %08x\n", channel, cs, rw == 1 ? "write" : "read", len, mar);
		write(2, line, n);
	}
	cr &= ~1u;      // done
	cpr |= 1u << 3; // TCINT
}
} // namespace

// set by a hardware write (not the GX FIFO's): the run loop looks at the devices and interrupt
// lines at once instead of at its next poll (ssa_run.cpp)
volatile uint32_t g_ssaKick = 0;

// the GX FIFO (write-gather pipe, 0xCC008000): the bytes into 32-byte bursts. Without the GX thread,
// BP register writes are picked out here (a byte 0x61, then the 32-bit register and value) for the
// pixel engine's draw-done and token interrupts, which GXDrawDone and GXSetDrawSync wait for (with it,
// the GX thread reports them as it decodes them: gxApplyPeEvents). Most hardware writes are these:
// mmioHwWrite comes straight here.
namespace
{
bool s_bpNext = false;
inline void fifoWrite(uint32_t value, int size)
{
	s_fifoBytes += size;
	static const bool threaded = gxThreaded();
	if (s_bpNext && size == 4 && !threaded)
	{
		const uint32_t reg = value >> 24;
		uint32_t& peWord = s_regs[0xCC001008];
		if (reg == 0x45) // PE_DONE: drawing finished
			peWord |= 1u << 3;
		if (reg == 0x45 || reg == 0x47 || reg == 0x48)
			s_gxSyncPending = true;
		else if (reg == 0x47 || reg == 0x48) // PE_TOKEN / PE_TOKEN_INT
		{
			s_regs[0xCC00100C] = (s_regs[0xCC00100C] & 0xFFFF0000u) | (value & 0xFFFF);
			if (reg == 0x48)
				peWord |= 1u << 2;
		}
	}
	s_bpNext = size == 1 && value == 0x61;
	// (the bytes, big-endian, into the burst)
	switch (size)
	{
	case 4:
	{
		const uint32_t be = __builtin_bswap32(value);
		memcpy(s_gather + s_gatherFill, &be, 4);
		break;
	}
	case 2:
	{
		const uint16_t be = __builtin_bswap16((uint16_t)value);
		memcpy(s_gather + s_gatherFill, &be, 2);
		break;
	}
	default:
		s_gather[s_gatherFill] = (uint8_t)value;
		break;
	}
	s_gatherFill += size;
	if (s_gatherFill >= 32)
	{
		gatherBurst(s_gather);
		s_gatherFill -= 32;
		memcpy(s_gather, s_gather + 32, 4); // (at most 3 bytes over)
	}
}
} // namespace

// (8) patch-aot-gp.py: translated code appends its gather-pipe writes to s_gather itself (the imports'
// gp/gpFill) and calls this for each full 32 bytes - only with the GX thread (no BP sniffing needed
// then) and without the access log or histogram; SSA_AOT_GP=0: through hwWrite, as before
void mmioGatherBurst()
{
	gatherBurst(s_gather);
	s_gatherFill -= 32;
	memcpy(s_gather, s_gather + 32, 4); // (at most 3 bytes over)
}
uint8_t* mmioGatherBuffer() { return s_gather; }
uint32_t* mmioGatherFill() { return &s_gatherFill; }
bool mmioGatherInline()
{
	const char* t = getenv("SSA_GX_THREAD");
	const char* g = getenv("SSA_AOT_GP");
	const char* h = getenv("SSA_MMIO_HIST");
	return t && atoi(t) && !getenv("SSA_GXREC") && !getenv("SSA_MMIO_LOG") && !(h && atoi(h)) && (!g || atoi(g));
}
uint64_t mmioGatherBursts() { return s_gatherBursts; }

void mmioDeviceWrite(uint32_t addr, uint32_t value, int size)
{
	const uint32_t a = canonical(addr);
	if ((a & ~31u) != 0xCC008000)
		flushBursts(); // (the batched FIFO bursts before anything else changes)
	if ((a & ~31u) != 0xCC008000)
		g_ssaKick = 1;
	if (iosOwns(a & ~3u))
	{
		iosWrite(a & ~3u, value);
		return;
	}
	if ((a & ~3u) == 0xCC003000)
	{
		s_piLatched &= ~value;
		return;
	}
	if (dspOwns(a))
	{
		dspWrite(a, value, size);
		return;
	}
	if ((a & ~31u) == 0xCC008000)
	{
		fifoWrite(value, size);
		return;
	}
	uint32_t& word = s_regs[a & ~3u];
	const uint32_t old = word;
	if (size == 4)
		word = value;
	else
	{
		const int shift = (4 - size - (int)(a & 3)) * 8;
		const uint32_t mask = ((1u << (size * 8)) - 1) << shift;
		word = (word & ~mask) | ((value << shift) & mask);
	}
	// the CP's registers: the GPU side follows control, pointer and breakpoint changes
	if (a >= kCp && a < kCp + 0x40)
		cpUpdate();
	// status bits that a write of 1 clears (and a write of 0 leaves as they were)
	auto writeOneToClear = [&](uint32_t bits) {
		const uint32_t ones = word & bits; // (the merged word: 1 where 1 was written to them)
		word = (word & ~bits) | (old & bits & ~ones);
	};
	// EXI: CPR's interrupt status bits (EXIINT bit 1, TCINT bit 3, EXTINT bit 11); CR's start bit runs
	// the transfer
	if (a >= 0xCD006800 && a < 0xCD006800 + 3 * 0x14)
	{
		const int channel = (int)((a - 0xCD006800) / 0x14);
		const uint32_t reg = (a - 0xCD006800) % 0x14 & ~3u;
		if (reg == 0)
			writeOneToClear((1u << 1) | (1u << 3) | (1u << 11));
		else if (reg == 0xC && (word & 1))
			exiTransfer(channel);
	}
	// DSP control/status (the low half of 0xCC005008, at 0xCC00500A): RES (bit 0) clears itself once
	// the reset is done; AIDINT (bit 3), ARINT (bit 5) and DSPINT (bit 7) are cleared by writing 1
	if ((a & ~3u) == 0xCC005008 && (size == 4 || (a & 2)))
	{
		writeOneToClear((1u << 3) | (1u << 5) | (1u << 7));
		// (the DSP's DMA busy bit: never busy; DSPInitCode (bit 10) is dsp.cpp's)
		word &= ~((1u << 9) | (1u << 10));
		// halt, reset, DSPInit: the DSP side reacts (microcode changes, mails)
		word = (word & 0xFFFF0000u) | (dspControlWritten(old & 0xFFFF, word & 0xFFFF) & 0xFFFF);
	}
	// AI control: the sample counter starts, stops and resets; SCRESET reads back as 0
	if ((a & ~3u) == 0xCD006C00)
	{
		aiControlWritten(old, word);
		word &= ~(1u << 5);
	}
	// PE interrupt control (the low half of 0xCC001008): token/finish status bits cleared by writing 1
	if ((a & ~3u) == 0xCC001008 && (size == 4 || (a & 2)))
	{
		writeOneToClear((1u << 2) | (1u << 3));
		static int s_peLogs = 0;
		if (getenv("SSA_PE_LOG") && s_peLogs++ < 20)
		{
			char line[128];
			const int n = snprintf(line, sizeof(line), "pe: write %08x size %d value %08x: old %08x now %08x\n", a, size, value, old, word);
			write(2, line, n);
		}
	}
	// ARAM DMA: the length written starts it; done at once
	if ((a & ~3u) == 0xCC005028)
		s_regs[0xCC005008] |= 1u << 5; // ARINT
}

uint64_t (*g_mmioTimeBase)() = nullptr; // (the runtime's time base, for counters)

uint64_t mmioAccessCount()
{
	return s_accesses;
}

// Instructions the decoder doesn't know (ALU operations with a memory operand, SSE loads, ...): the
// register page is mapped for one instruction, filled with the registers' current values, and the
// instruction runs for real with the trap flag set; the trap after it (mmioTrap) turns the words it
// changed into device writes and unmaps the page again. Exact for any form, flags included.
extern uint8_t* memory_base;
#include <sys/mman.h>
namespace
{
uint8_t* s_stepPage = nullptr;      // the host page mapped for the step (nullptr: none)
uint32_t s_stepGuestPage = 0;
uint8_t s_stepBefore[hostctx::kPage];
uint64_t s_steps = 0;
}

// Whether an instruction writes its memory operand, and with what size: a step can't see a write of
// the value that was already there (an acknowledge that sets an already-set write-1-to-clear bit:
// clang turns lhz/ori/sth into `or word [mem], imm`), so these writes are passed on regardless.
static bool memoryWrite(const uint8_t* p, int& size)
{
	bool op16 = false, rexW = false;
	for (;; p++)
	{
		if (*p == 0x66) op16 = true;
		else if (*p == 0xF0 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36) {}
		else break;
	}
	if ((*p & 0xF0) == 0x40)
		rexW = *p++ & 8;
	const int word = rexW ? 8 : (op16 ? 2 : 4);
	const uint8_t op = *p++;
	const int reg = (*p >> 3) & 7; // (ModRM's reg field: the operation for group opcodes)
	if (op < 0x40 && (op & 7) <= 1 && (op & 0x38) != 0x38) // ALU r/m, reg (not CMP)
		return size = (op & 1) ? word : 1, true;
	if ((op == 0x80 || op == 0x81 || op == 0x83) && reg != 7) // ALU r/m, imm (not CMP)
		return size = op == 0x80 ? 1 : word, true;
	if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) // shifts and rotates
		return size = (op & 1) ? word : 1, true;
	if ((op == 0xF6 || op == 0xF7) && (reg == 2 || reg == 3)) // NOT, NEG
		return size = op == 0xF7 ? word : 1, true;
	if ((op == 0xFE || op == 0xFF) && reg <= 1) // INC, DEC
		return size = op == 0xFF ? word : 1, true;
	if (op == 0x86 || op == 0x87) // XCHG
		return size = op == 0x87 ? word : 1, true;
	if (op == 0x0F)
	{
		const uint8_t op2 = p[-1 + 1];
		const int reg2 = (p[1] >> 3) & 7;
		if (op2 == 0xB0 || op2 == 0xC0) return size = 1, true;   // CMPXCHG, XADD (byte)
		if (op2 == 0xB1 || op2 == 0xC1) return size = word, true; // CMPXCHG, XADD
		if (op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) return size = word, true; // BTS, BTR, BTC
		if (op2 == 0xBA && reg2 >= 5) return size = word, true;   // BTS/BTR/BTC imm
	}
	return false;
}

namespace
{
uint32_t s_stepAddr = 0;
int s_stepWriteSize = 0; // the instruction writes this many bytes at s_stepAddr (0: no write)
}

static bool singleStep(void* uc, uint32_t addr, uint32_t guestIp, void* hostAt)
{
	if (s_stepPage)
		return false; // (a second page in the same instruction: not handled)
	constexpr uint32_t kMask = (uint32_t)hostctx::kPage - 1;
	const uint32_t page = addr & ~kMask;
	// (the page that faulted: the register area's own, or its physical alias)
	uint8_t* host = (uint8_t*)((uintptr_t)hostAt & ~(uintptr_t)kMask);
	if (!hostPageMap(host))
		return false;
	// (the registers are kept by canonical address: a page of the 0xCD80xxxx mirror gets its copy)
	const uint32_t regPage = (page & 0xFF800000) == 0xCD800000 ? page & ~0x00800000u : page;
	s_regs.forEach([&](uint32_t a, uint32_t v) {
		if ((a & ~kMask) == regPage)
			*(uint32_t*)(host + (a & kMask)) = __builtin_bswap32(v);
	});
	// (registers with their own logic: their current values)
	const uint32_t canonicalPage = (page & 0xFF800000) == 0xCD800000 ? page & ~0x00800000u : page;
	if (canonicalPage == (0xCD000000 & ~kMask))
		for (uint32_t r : {0x00u, 0x04u, 0x08u, 0x30u, 0x34u, 0x64u})
			*(uint32_t*)(host + ((0xCD000000 + r) & kMask)) = __builtin_bswap32(iosRead(0xCD000000 + r));
	if (canonicalPage == (0xCC003000 & ~kMask))
		*(uint32_t*)(host + (0xCC003000 & kMask)) = __builtin_bswap32(piCause());
	memcpy(s_stepBefore, host, hostctx::kPage);
	// (the first instruction forms that come here, for the decoder: a step can't see a write of the
	// value already there)
	static int s_forms = 0;
	if (s_forms < 12)
	{
		const uint8_t* rip = (const uint8_t*)hostctx::rip(uc);
		char line[160];
		const int n = snprintf(line, sizeof(line), "mmio: single-stepped at %08x: %02x %02x %02x %02x %02x %02x %02x %02x\n", addr, rip[0], rip[1],
			rip[2], rip[3], rip[4], rip[5], rip[6], rip[7]);
		write(2, line, n);
		s_forms++;
	}
	s_stepPage = host;
	s_stepGuestPage = page;
	s_stepAddr = addr;
	int writeSize = 0;
	s_stepWriteSize = memoryWrite((const uint8_t*)hostctx::rip(uc), writeSize) ? writeSize : 0;
	s_steps++;
	logAccess(false, addr, 4, __builtin_bswap32(*(uint32_t*)(host + (addr & kMask & ~3u))), guestIp);
	hostctx::flags(uc) |= 0x100; // trap flag: back after one instruction
	return true;
}

// the trap after a single-stepped instruction: true if it was one of ours
bool mmioTrap(void* context)
{
	if (!s_stepPage)
		return false;
	hostctx::flags(context) &= ~0x100ull;
	// the instruction's own write, as wide as it was, even if the value didn't change
	const uint32_t off0 = s_stepAddr & ((uint32_t)hostctx::kPage - 1);
	if (s_stepWriteSize == 1 || s_stepWriteSize == 2 || s_stepWriteSize == 4)
	{
		uint32_t value = 0;
		for (int i = 0; i < s_stepWriteSize; i++)
			value = value << 8 | s_stepPage[off0 + i];
		logAccess(true, s_stepAddr, s_stepWriteSize, value, 0);
		mmioDeviceWrite(s_stepAddr, value, s_stepWriteSize);
	}
	// anything else the instruction changed, word by word
	for (uint32_t off = 0; off < hostctx::kPage; off += 4)
	{
		if (s_stepWriteSize && off < off0 + s_stepWriteSize && off + 4 > off0)
			continue;
		if (memcmp(s_stepPage + off, s_stepBefore + off, 4) != 0)
		{
			const uint32_t value = __builtin_bswap32(*(uint32_t*)(s_stepPage + off));
			logAccess(true, s_stepGuestPage + off, 4, value, 0);
			mmioDeviceWrite(s_stepGuestPage + off, value, 4);
		}
	}
	hostPageUnmap(s_stepPage);
	s_stepPage = nullptr;
	return true;
}

uint64_t mmioStepCount()
{
	return s_steps;
}

// SSA_MMIO_HIST=1: hardware accesses counted by register (0xCC00xxxx / 0xCD00xxxx words) and by the
// guest address the dispatcher last entered; the top ones every 10 s (mmioHistReport)
namespace
{
bool s_hist = false;
uint32_t s_histRegs[2][0x4000];
constexpr int kHistIps = 4096;
uint32_t s_histIpKey[kHistIps], s_histIpCount[kHistIps];

void histCount(uint32_t addr, uint32_t guestIp)
{
	s_histRegs[(addr >> 24) & 1][(addr & 0xFFFF) >> 2]++;
	uint32_t h = (guestIp * 2654435761u) >> 20;
	for (int i = 0; i < 8; i++, h = (h + 1) & (kHistIps - 1))
		if (s_histIpKey[h] == guestIp || s_histIpKey[h] == 0)
		{
			s_histIpKey[h] = guestIp;
			s_histIpCount[h]++;
			return;
		}
}

// trapped accesses by the translated function that made them (1: not translated code)
constexpr int kTraps = 1024;
uint32_t s_trapKey[kTraps], s_trapCount[kTraps];
void trapCount(uint32_t entry)
{
	uint32_t h = (entry * 2654435761u) >> 22;
	for (int i = 0; i < 16; i++, h = (h + 1) & (kTraps - 1))
		if (s_trapKey[h] == entry || s_trapKey[h] == 0)
		{
			s_trapKey[h] = entry;
			s_trapCount[h]++;
			return;
		}
}
} // namespace
bool CremaAotOwnerEntry(uint64_t rip, uint32_t& entry); // patch-crema-wii.py

void mmioHistReport()
{
	if (!s_hist)
		return;
	char line[2048];
	int n = snprintf(line, sizeof(line), "mmio: busiest registers (count since the last report):");
	for (int k = 0; k < 10; k++)
	{
		int bb = -1, bi = 0;
		uint32_t best = 0;
		for (int b = 0; b < 2; b++)
			for (int i = 0; i < 0x4000; i++)
				if (s_histRegs[b][i] > best)
					best = s_histRegs[b][i], bb = b, bi = i;
		if (bb < 0)
			break;
		n += snprintf(line + n, sizeof(line) - n, " %08x (%s) %u;", (bb ? 0xCD000000u : 0xCC000000u) | (bi << 2),
			blockOf((bb ? 0xCD000000u : 0xCC000000u) | (bi << 2)), best);
		s_histRegs[bb][bi] = 0;
	}
	memset(s_histRegs, 0, sizeof(s_histRegs));
	n += snprintf(line + n, sizeof(line) - n, "\nmmio: busiest guest code (last dispatcher entry):");
	for (int k = 0; k < 8; k++)
	{
		int bi = -1;
		uint32_t best = 0;
		for (int i = 0; i < kHistIps; i++)
			if (s_histIpCount[i] > best)
				best = s_histIpCount[i], bi = i;
		if (bi < 0)
			break;
		n += snprintf(line + n, sizeof(line) - n, " %08x %u;", s_histIpKey[bi], best);
		s_histIpCount[bi] = 0;
	}
	memset(s_histIpKey, 0, sizeof(s_histIpKey));
	memset(s_histIpCount, 0, sizeof(s_histIpCount));
	// every function that trapped (kept, not reset: the whole run's list), as "mmio: trapped in <entry>
	// <count>" lines for patch-aot-hw.py select
	n += snprintf(line + n, sizeof(line) - n, "\n");
	write(2, line, n);
	n = 0;
	for (int i = 0; i < kTraps; i++)
		if (s_trapCount[i])
		{
			n += snprintf(line + n, sizeof(line) - n, "mmio: trapped in %08x %u\n", s_trapKey[i], s_trapCount[i]);
			if (n > (int)sizeof(line) - 64)
			{
				write(2, line, n);
				n = 0;
			}
		}
	write(2, line, n);
}

// the diagnostic switches (SSA_MMIO_LOG, SSA_MMIO_HIST), read at reset: after the PS4's env.txt
void mmioReadSwitches()
{
	s_logAll = getenv("SSA_MMIO_LOG") != nullptr;
	s_hist = getenv("SSA_MMIO_HIST") && atoi(getenv("SSA_MMIO_HIST"));
}

// translated code's hardware register accesses, called directly (patch-aot-hw.py: the imports'
// hwRead/hwWrite) instead of through a fault: the same device emulation, no trap
extern volatile uint32_t g_ssaHwIp; // ssa_run.cpp: the guest address the dispatcher last entered
uint64_t s_directAccesses = 0;
uint32_t mmioHwRead(uint32_t addr, uint32_t size)
{
	s_accesses++;
	s_directAccesses++;
	if (s_hist)
		histCount(addr, g_ssaHwIp);
	const uint32_t value = mmioDeviceRead(addr, (int)size);
	logAccess(false, addr, (int)size, value, g_ssaHwIp);
	return value;
}

void mmioHwWrite(uint32_t addr, uint32_t value, uint32_t size)
{
	s_accesses++;
	s_directAccesses++;
	// (the GX FIFO: most writes; straight into the gather pipe)
	if ((addr & ~31u) == 0xCC008000 && !s_hist && !s_logAll)
	{
		fifoWrite(value, (int)size);
		return;
	}
	if (s_hist)
		histCount(addr, g_ssaHwIp);
	logAccess(true, addr, (int)size, value, g_ssaHwIp);
	mmioDeviceWrite(addr, value, (int)size);
}

uint64_t mmioDirectCount()
{
	return s_directAccesses;
}

// a fault at guest address addr in the hardware register area: emulated (true) or not handled
bool mmioEmulate(void* context, uint32_t addr, uint32_t guestIp, void* hostAt)
{
	if (s_hist)
	{
		histCount(addr, guestIp);
		// (trapped: which translated function did it, for patch-aot-hw.py select's extra list)
		uint32_t entry = 0;
		if (!CremaAotOwnerEntry((uint64_t)hostctx::rip(context), entry))
			entry = 1; // (not translated code: the interpreter or the runtime)
		trapCount(entry);
	}
	void* uc = context;
	const uint8_t* rip = (const uint8_t*)hostctx::rip(uc);
	Access a;
	if (!decode(rip, a))
	{
		s_accesses++;
		if (singleStep(uc, addr, guestIp, hostAt))
			return true;
		char line[160];
		const int n = snprintf(line, sizeof(line), "mmio: can't handle the access at %08x: %02x %02x %02x %02x %02x %02x %02x %02x\n", addr, rip[0],
			rip[1], rip[2], rip[3], rip[4], rip[5], rip[6], rip[7]);
		write(2, line, n);
		return false;
	}
	s_accesses++;
	// (accesses that don't fit their alignment: the instruction, once each, for diagnosis)
	if ((addr & (a.size - 1)) != 0)
	{
		static int s_odd = 0;
		if (s_odd++ < 6)
		{
			char line[160];
			const int n = snprintf(line, sizeof(line), "mmio: unaligned %s%d at %08x by host %p: %02x %02x %02x %02x %02x %02x %02x %02x\n", a.store ? "W" : "R",
				a.size * 8, addr, rip, rip[0], rip[1], rip[2], rip[3], rip[4], rip[5], rip[6], rip[7]);
			write(2, line, n);
		}
	}
	if (a.store)
	{
		uint32_t raw;
		if (a.immediate)
			raw = (uint32_t)a.imm;
		else if (a.high8)
			raw = (uint32_t)(hostctx::reg(uc, a.reg) >> 8) & 0xFF;
		else
			raw = (uint32_t)hostctx::reg(uc, a.reg);
		const uint32_t value = a.movbe ? (raw & (a.size == 4 ? 0xFFFFFFFFu : (1u << (a.size * 8)) - 1)) : swapSize(raw, a.size);
		logAccess(true, addr, a.size, value, guestIp);
		mmioDeviceWrite(addr, value, a.size);
	}
	else
	{
		const uint32_t value = mmioDeviceRead(addr, a.size);
		logAccess(false, addr, a.size, value, guestIp);
		uint64_t raw = a.movbe ? value : swapSize(value, a.size);
		if (a.signExtend)
			raw = a.size == 1 ? (uint64_t)(int64_t)(int8_t)raw : (uint64_t)(int64_t)(int16_t)raw;
		uint64_t& r = hostctx::reg(uc, a.reg);
		if (a.high8)
			r = (r & ~0xFF00ull) | ((raw & 0xFF) << 8);
		else if (a.destSize == 8)
			r = raw;
		else if (a.destSize == 4)
			r = (uint32_t)raw; // (a 32-bit write clears the upper half)
		else if (a.destSize == 2)
			r = (r & ~0xFFFFull) | (raw & 0xFFFF);
		else
			r = (r & ~0xFFull) | (raw & 0xFF);
	}
	hostctx::rip(uc) += a.length;
	return true;
}
