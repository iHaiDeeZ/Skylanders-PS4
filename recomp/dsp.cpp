// Skylanders runtime: the audio DSP, high-level emulated, and the audio DMA that drives it.
// Behaviour after Dolphin's DSPHLE (DSPHLE.cpp, MailHandler.cpp, UCodes/ROM.cpp, INIT, AX.cpp,
// AXWii.cpp; GPL-2.0-or-later, as this file).
//
// Registers (0xCC005000), the DSP's side of them here; the control register (CSR, 0xCC00500A) stays
// in mmio.cpp, which calls dspControlWritten/dspControlRead:
//   0x5000/0x5002  CPU->DSP mail: writing the low half delivers it to the microcode; the busy bit
//                  (bit 15 of the high half) clears at once
//   0x5004/0x5006  DSP->CPU mail: the next queued mail while the DSP isn't halted; reading the low
//                  half takes it (and raises the DSP interrupt if the mail carries one)
//   0x5030-0x503A  audio DMA: start address (hi/lo), control/length (bit 15 start, 32-byte blocks),
//                  blocks left. A started buffer plays out at 32 kHz; at its end the AI DMA
//                  interrupt (CSR AIDINT, bit 3) is raised and the next buffer starts.
// Microcode:
//   ROM   (after reset): mails 0x8071FEED, takes the boot parameters (0x80F3A001 RAM address,
//         0x80F3A002 length, 0x80F3B002 DMEM length, 0x80F3C002 IMEM address, 0x80F3D001 start PC)
//         and boots: the code's Ector hash picks the HLE microcode (AXWii for a Wii game).
//   INIT  (DSPInit cleared): mails 0x80544348 (the OS's __OSInitAudioSystem).
//   AXWii: mails DSP_INIT (0xDCD10000); per audio frame the CPU mails 0xBABE0000 | list size, then
//         the list's address; the list is walked to CMD_END and DSP_YIELD (0xDCD10002) is mailed with
//         an interrupt. Task mails (0xCDD1xxxx): resume, new microcode, reset, continue.
//   v1: voices aren't processed or mixed yet (outputs are silence, voice state doesn't advance).

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "settings.h"
#include <stdlib.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <utility>
#include <vector>

extern uint8_t* memory_base;
void mmioCsrSet(uint32_t bits);          // mmio.cpp: CSR status bits (DSPINT bit 7, AIDINT bit 3)
extern uint64_t (*g_mmioTimeBase)();     // mmio.cpp: the runtime's time base
// the host's audio output (ps4/ps4_audio.cpp sets it): host-order left/right 16-bit pairs at 32 kHz
void (*g_audioSink)(const int16_t* lr, uint32_t frames) = nullptr;

namespace
{
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...)
{
	char line[256];
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	if (n < 0)
		return;
	n = n > (int)sizeof(line) - 2 ? (int)sizeof(line) - 2 : n;
	line[n++] = '\n';
	write(2, line, n);
}

uint64_t now() { return g_mmioTimeBase ? g_mmioTimeBase() : 0; }
uint16_t rd16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(memory_base + a)); }

// ---- the mail handler ----
std::deque<std::pair<uint32_t, bool>> s_toCpu; // (mail, raise the DSP interrupt when it is read)
uint32_t s_lastMail = 0;
bool s_halted = true;
uint32_t s_cpuMailbox = 0;
std::vector<std::pair<uint64_t, uint32_t>> s_delayedInterrupts; // DSP interrupts due at a time base
int s_mailLogs = 60;

void dspInterrupt()
{
	mmioCsrSet(1u << 7); // DSPINT
}

void pushMail(uint32_t mail, bool interrupt, uint64_t delayTicks = 0)
{
	if (interrupt)
	{
		if (s_toCpu.empty())
		{
			if (delayTicks)
				s_delayedInterrupts.push_back({now() + delayTicks, 0});
			else
				dspInterrupt();
		}
		else
			s_toCpu.front().second = true;
	}
	s_toCpu.push_back({mail, false});
	if (s_mailLogs > 0)
	{
		s_mailLogs--;
		log("dsp: mail to the CPU %08x%s", mail, interrupt ? " (+ interrupt)" : "");
	}
}

// ---- microcode ----
enum class UCode { Rom, Init, AXWii, None };
UCode s_ucode = UCode::None;
bool s_oldAxWii = false;
uint32_t s_crc = 0;

// ROM boot parameters
uint32_t s_romNext = 0, s_romRam = 0, s_romLength = 0;

// SSA_AUDIO=0: the voices aren't mixed (silence, the old way)
const bool s_mixOn = [] {
	const char* e = getenv("SSA_AUDIO");
	return !e || atoi(e) != 0;
}();

// AX state
enum class AxState { WaitingForCmdListSize, WaitingForCmdListAddress, WaitingForNextTask };
AxState s_axState = AxState::WaitingForCmdListSize;
uint16_t s_cmdListSize = 0;
bool s_uploadSetup = false;
int s_uploadSteps = 0;
uint32_t s_uploadIramAddr = 0, s_uploadIramSize = 0;
bool s_needsResumeMail = false;
uint64_t s_axFrames = 0;

uint32_t hashEctor(const uint8_t* data, size_t len)
{
	uint32_t crc = 0;
	for (size_t i = 0; i < len; i++)
	{
		crc ^= data[i];
		crc = (crc << 3) | (crc >> 29);
	}
	return crc;
}

void setUCode(UCode u)
{
	s_toCpu.clear();
	s_ucode = u;
	switch (u)
	{
	case UCode::Rom:
		s_romNext = 0;
		pushMail(0x8071FEED, false);
		break;
	case UCode::Init:
		pushMail(0x80544348, false);
		break;
	case UCode::AXWii:
		s_axState = AxState::WaitingForCmdListSize;
		pushMail(0xDCD10000, true); // DSP_INIT
		break;
	case UCode::None:
		break;
	}
}

void bootByHash(uint32_t address, uint32_t length, const char* how)
{
	const uint8_t* code = memory_base + address;
	s_crc = hashEctor(code, length);
	// (Dolphin's list: these two are the older AXWii command set)
	s_oldAxWii = s_crc == 0xfa450138 || s_crc == 0x7699af32;
	log("dsp: %s microcode at %08x, %u bytes, hash %08x: AXWii%s", how, address, length, s_crc, s_oldAxWii ? " (old commands)" : "");
	setUCode(UCode::AXWii);
}

void romMail(uint32_t mail)
{
	if (s_romNext == 0)
	{
		if ((mail & 0xFFFF0000) != 0x80F30000)
			pushMail(0xFEEE0000 | (mail & 0xFFFF), false);
		else
			s_romNext = mail;
		return;
	}
	switch (s_romNext)
	{
	case 0x80F3A001: s_romRam = mail; break;
	case 0x80F3A002: s_romLength = mail & 0xFFFF; break;
	case 0x80F3D001:
		bootByHash(s_romRam, s_romLength, "boot");
		return;
	default: break; // DMEM length, IMEM address
	}
	s_romNext = 0;
}

// ---- AXWii voice mixing (after Dolphin's AXWii.cpp and AXVoice.h) ----
// An AX frame is 3 ms: 96 samples at 32 kHz. The mix buffers hold 32-bit sums; CMD_OUTPUT clamps the
// main ones to 16 bits into RAM, which the audio DMA then plays.
constexpr int kSpf = 96;
enum { ML, MR, MS, AL, AR, AS, BL, BR, BS, CL, CR, CS, kBufs };
int32_t s_mix[kBufs][kSpf];
uint16_t s_lastMainVolume = 0x8000, s_lastAuxVolume[3] = {0x8000, 0x8000, 0x8000};
uint64_t s_voicesMixed = 0;

// a guest address (physical, or a cached / uncached mirror) -> host; MEM1 and MEM2 only
uint8_t* ram(uint32_t a, uint32_t bytes)
{
	a &= 0x1FFFFFFF;
	if (a + bytes <= 0x01800000 || (a >= 0x10000000 && a + bytes <= 0x14000000))
		return memory_base + 0x80000000u + a;
	return nullptr;
}
int32_t ram32(const uint8_t* p) { return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]); }
void put32(uint8_t* p, int32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}
void put16(uint8_t* p, int16_t v)
{
	p[0] = (uint8_t)((uint16_t)v >> 8);
	p[1] = (uint8_t)v;
}
int32_t clamp16(int64_t v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : (int32_t)v; }

// the accelerator's memory: on the Wii, physical RAM (bit 28: MEM2)
uint8_t aram(uint32_t a)
{
	if (a & 0x10000000)
	{
		a &= 0x03FFFFFF;
		return memory_base[0x90000000u + a];
	}
	a &= 0x01FFFFFF;
	return a < 0x01800000 ? memory_base[0x80000000u + a] : 0;
}

// the setup command: every buffer starts at a value plus a per-sample delta (12 buffers of 96 samples,
// then the Wii Remote ones, not mixed here)
void axSetup(uint32_t addr)
{
	const uint8_t* p = ram(addr, 36 * 2);
	if (!p)
		return;
	for (int b = 0; b < kBufs; b++)
	{
		int32_t init = (int32_t)((uint32_t)(p[6 * b] << 8 | p[6 * b + 1]) << 16 | (uint32_t)(p[6 * b + 2] << 8 | p[6 * b + 3]));
		const int16_t delta = (int16_t)(p[6 * b + 4] << 8 | p[6 * b + 5]);
		for (int i = 0; i < kSpf; i++, init += delta)
			s_mix[b][i] = init;
	}
}

void axAddToLR(uint32_t addr, bool neg)
{
	const uint8_t* p = ram(addr, kSpf * 4);
	if (!p)
		return;
	for (int i = 0; i < kSpf; i++)
	{
		const int32_t v = neg ? -ram32(p + 4 * i) : ram32(p + 4 * i);
		s_mix[ML][i] += v;
		s_mix[MR][i] += v;
	}
}

void axAddSubToLR(uint32_t addr)
{
	const uint8_t* p = ram(addr, kSpf * 8);
	if (!p)
		return;
	for (int i = 0; i < kSpf; i++)
	{
		s_mix[ML][i] += ram32(p + 4 * i);
		s_mix[MR][i] -= ram32(p + 4 * (kSpf + i));
	}
}

void volumeRamp(uint16_t* out, uint16_t from, uint16_t to)
{
	float v = from;
	for (int i = 0; i < kSpf; i++)
	{
		v += ((int)to - (int)from) / (float)kSpf;
		out[i] = (uint16_t)v;
	}
}

// an AUX bus: its samples to the CPU (for its effects), the CPU's processed samples back into main
void axMixAux(int aux, uint32_t writeAddr, uint32_t readAddr, uint16_t volume)
{
	uint16_t ramp[kSpf];
	volumeRamp(ramp, s_lastAuxVolume[aux], volume);
	s_lastAuxVolume[aux] = volume;
	const int first = AL + 3 * aux;
	if (writeAddr)
		if (uint8_t* w = ram(writeAddr, 3 * kSpf * 4))
			for (int b = 0; b < 3; b++)
				for (int i = 0; i < kSpf; i++)
					put32(w + 4 * (b * kSpf + i), s_mix[first + b][i]);
	const uint8_t* r = ram(readAddr, 3 * kSpf * 4);
	if (!r)
		return;
	for (int b = 0; b < 3; b++)
		for (int i = 0; i < kSpf; i++)
			s_mix[ML + b][i] += (int32_t)(((int64_t)ram32(r + 4 * (b * kSpf + i)) * ramp[i]) >> 15);
}

void axUploadAuxMixLRSC(int aux, const uint32_t* addr, uint16_t volume)
{
	const int l = aux ? BL : AL, r = aux ? BR : AR, s = aux ? BS : AS, c = aux ? CS : CR;
	if (uint8_t* w = ram(addr[0], 3 * kSpf * 4))
		for (int i = 0; i < kSpf; i++)
		{
			put32(w + 4 * i, s_mix[l][i]);
			put32(w + 4 * (kSpf + i), s_mix[r][i]);
			put32(w + 4 * (2 * kSpf + i), s_mix[s][i]);
		}
	if (uint8_t* w = ram(addr[1], kSpf * 4))
		for (int i = 0; i < kSpf; i++)
			put32(w + 4 * i, s_mix[c][i]);
	uint16_t ramp[kSpf];
	volumeRamp(ramp, s_lastAuxVolume[aux], volume);
	s_lastAuxVolume[aux] = volume;
	const int dest[4] = {ML, MR, MS, CL};
	for (int m = 0; m < 4; m++)
		if (const uint8_t* p = ram(addr[2 + m], kSpf * 4))
			for (int i = 0; i < kSpf; i++)
				s_mix[dest[m]][i] += (int32_t)(((int64_t)ram32(p + 4 * i) * ramp[i]) >> 15);
}

// the main mix to RAM: 16-bit stereo (right, left: the audio DMA's order), and the surround sums
void axOutput(uint32_t lrAddr, uint32_t surroundAddr, uint16_t volume)
{
	static int outLog = getenv("SSA_DSP_VOICELOG") ? 20 : 0;
	if (outLog > 0)
	{
		int32_t peak = 0;
		for (int i = 0; i < kSpf; i++)
			peak = std::max(peak, std::max(std::abs(s_mix[ML][i]), std::abs(s_mix[MR][i])));
		if (peak)
		{
			outLog--;
			log("dsp: output to %08x volume %04x, main mix peak %d", lrAddr, volume, peak);
		}
	}
	if (uint8_t* s = ram(surroundAddr, kSpf * 4))
		for (int i = 0; i < kSpf; i++)
			put32(s + 4 * i, s_mix[MS][i]);
	uint16_t ramp[kSpf];
	volumeRamp(ramp, s_lastMainVolume, volume);
	s_lastMainVolume = volume;
	uint8_t* o = ram(lrAddr, kSpf * 4);
	if (!o)
		return;
	for (int i = 0; i < kSpf; i++)
	{
		const int32_t l = clamp16(((int64_t)s_mix[ML][i] * ramp[i]) >> 15), r = clamp16(((int64_t)s_mix[MR][i] * ramp[i]) >> 15);
		put16(o + 4 * i, (int16_t)r);
		put16(o + 4 * i + 2, (int16_t)l);
	}
}

// ---- one voice (an AXPBWii, 160 big-endian words) ----
enum PbWord
{
	kNextHi = 0, kSrcType = 4, kMixCtrlHi = 6, kRunning = 8, kIsStream = 9, kMixer = 10, kDpop = 41, kVolEnv = 53, kLooping = 55, kFormat = 56,
	kLoopHi = 57, kEndHi = 59, kCurHi = 61, kCoefs = 63, kPredScale = 80, kYn1 = 81, kYn2 = 82, kRatioHi = 83, kFrac = 85, kLast = 86,
	kLoopPredScale = 90, kLoopYn1 = 91, kLoopYn2 = 92, kLpf = 93, kPbWords = 160
};

struct Voice
{
	uint16_t w[kPbWords];
	uint32_t at(int i) const { return (uint32_t)w[i] << 16 | w[i + 1]; }
	void set(int i, uint32_t v)
	{
		w[i] = (uint16_t)(v >> 16);
		w[i + 1] = (uint16_t)v;
	}
};

// the accelerator for one voice: reads ADPCM / PCM16 / PCM8 samples, loops at the end
struct Accel
{
	Voice& v;
	uint32_t start, end, cur;
	uint16_t format, predScale;
	int16_t yn1, yn2;
	bool stopped = false;

	explicit Accel(Voice& voice) : v(voice)
	{
		start = v.at(kLoopHi);
		end = v.at(kEndHi);
		cur = v.at(kCurHi);
		format = v.w[kFormat];
		predScale = v.w[kPredScale];
		yn1 = (int16_t)v.w[kYn1];
		yn2 = (int16_t)v.w[kYn2];
	}

	int16_t read()
	{
		if (stopped)
			return 0;
		int32_t val = 0;
		uint32_t step = 2;
		switch (format)
		{
		case 0x00: // ADPCM: 8-byte frames, a predictor/scale byte then 14 nibbles
		{
			if ((cur & 15) == 0)
			{
				predScale = aram((cur & ~15u) >> 1);
				cur += 2;
			}
			switch (end & 15)
			{
			case 0: step = 1; break;
			case 1: step = 0; break;
			default: step = 2; break;
			}
			const int scale = 1 << (predScale & 15), idx = (predScale >> 4) & 7;
			const int32_t c1 = (int16_t)v.w[kCoefs + 2 * idx], c2 = (int16_t)v.w[kCoefs + 2 * idx + 1];
			const uint8_t b = aram(cur >> 1);
			int t = (cur & 1) ? (b & 15) : (b >> 4);
			if (t >= 8)
				t -= 16;
			val = scale * t + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
			val = val < -0x7FFF ? -0x7FFF : val > 0x7FFF ? 0x7FFF : val;
			cur++;
			break;
		}
		case 0x0A: // PCM16 (addresses in samples)
			val = (int16_t)(aram(cur * 2) << 8 | aram(cur * 2 + 1));
			cur++;
			break;
		case 0x19: // PCM8
			val = (int16_t)(aram(cur) << 8);
			cur++;
			break;
		default:
		{
			static int unknown = 0;
			if (unknown++ < 5)
				log("dsp: voice sample format %04x not handled", format);
			cur++;
			break;
		}
		}
		yn2 = yn1;
		yn1 = (int16_t)val;
		if (cur == end + step - 1)
		{
			cur = start;
			if (v.w[kLooping])
			{
				predScale = v.w[kLoopPredScale];
				if (!v.w[kIsStream])
				{
					yn1 = (int16_t)v.w[kLoopYn1];
					yn2 = (int16_t)v.w[kLoopYn2];
				}
				else
				{
					v.w[kYn1] = (uint16_t)yn1;
					v.w[kYn2] = (uint16_t)yn2;
				}
			}
			else
			{
				// a one-shot voice ends (AXWii: the voice stops, its later reads are zeros)
				v.w[kRunning] = 0;
				v.w[kSrcType] = 2;
				stopped = true;
			}
		}
		return (int16_t)val;
	}
};

void mixAdd(int32_t* out, const int16_t* in, Voice& v, int volWord, int dpopWord, bool ramp)
{
	uint16_t vol = v.w[volWord];
	const int16_t delta = ramp ? (int16_t)v.w[volWord + 1] : 0;
	int32_t s = 0;
	for (int i = 0; i < kSpf; i++)
	{
		s = clamp16(((int64_t)in[i] * vol) >> 15);
		out[i] += (int16_t)s;
		vol = (uint16_t)(vol + delta);
	}
	v.w[volWord] = vol;
	v.w[dpopWord] = (uint16_t)(int16_t)s;
}

bool s_muted = false; // the quick menu's Sound Off (read once per AX frame)

void processVoice(Voice& v)
{
	// (SSA_DSP_VOICELOG=n: the first n running voices' parameter blocks)
	static int voiceLog = getenv("SSA_DSP_VOICELOG") ? atoi(getenv("SSA_DSP_VOICELOG")) : 0;
	if (voiceLog > 0 && v.w[kRunning])
	{
		voiceLog--;
		log("dsp: voice %08x: running %u stream %u src %u ratio %08x mixctl %08x main %04x/%04x %04x/%04x env %04x/%04x fmt %04x loop %u %08x end %08x cur %08x ps %04x lpf %u",
			v.at(2), v.w[kRunning], v.w[kIsStream], v.w[kSrcType], v.at(kRatioHi), v.at(kMixCtrlHi), v.w[kMixer], v.w[kMixer + 1], v.w[kMixer + 2],
			v.w[kMixer + 3], v.w[kVolEnv], v.w[kVolEnv + 1], v.w[kFormat], v.w[kLooping], v.at(kLoopHi), v.at(kEndHi), v.at(kCurHi), v.w[kPredScale], v.w[kLpf]);
		char line[200];
		int o = 0;
		const uint32_t cur = v.at(kCurHi);
		for (int i = 0; i < 24 && o < 190; i++)
		{
			const uint32_t a = v.w[kFormat] == 0x0A ? (cur + i) * 2 : v.w[kFormat] == 0x19 ? cur + i : (cur >> 1) + i;
			o += snprintf(line + o, sizeof(line) - o, v.w[kFormat] == 0x0A ? "%02x%02x " : "%02x ", aram(a), v.w[kFormat] == 0x0A ? aram(a + 1) : 0);
		}
		log("    data at cur: %s", line);
	}
	if (v.w[kRunning] != 1)
		return;
	s_voicesMixed++;
	// input samples, sample-rate converted (polyphase without the ROM's coefficients: linear, as
	// Dolphin does without them; type 2: none, 1:1)
	int16_t smp[kSpf];
	{
		Accel acc(v);
		const uint16_t type = v.w[kSrcType];
		uint32_t ratio = type == 2 ? 0x10000 : v.at(kRatioHi);
		if (ratio > 0x40000)
			ratio = 0x40000;
		int16_t h[4];
		for (int k = 0; k < 4; k++)
			h[k] = (int16_t)v.w[kLast + k];
		uint32_t pos = v.w[kFrac];
		for (int i = 0; i < kSpf; i++)
		{
			pos += ratio;
			while (pos >= 0x10000)
			{
				h[0] = h[1];
				h[1] = h[2];
				h[2] = h[3];
				h[3] = acc.read();
				pos -= 0x10000;
			}
			smp[i] = type == 2 ? h[3] : (int16_t)(h[2] + (((int32_t)h[3] - h[2]) * (int32_t)pos >> 16));
		}
		v.w[kFrac] = (uint16_t)pos;
		for (int k = 0; k < 4; k++)
			v.w[kLast + k] = (uint16_t)h[k];
		v.set(kCurHi, acc.cur);
		v.w[kYn1] = (uint16_t)acc.yn1;
		v.w[kYn2] = (uint16_t)acc.yn2;
		v.w[kPredScale] = acc.predScale;
	}
	// Sound Off: nothing mixed. The voice's place moved on above (it still ends and loops as it would);
	// its envelope and volume ramps move as mixing would move them.
	if (s_muted)
	{
		v.w[kVolEnv] = (uint16_t)(v.w[kVolEnv] + (int16_t)v.w[kVolEnv + 1] * kSpf);
		return;
	}
	// the volume envelope
	uint16_t env = v.w[kVolEnv];
	const int16_t envDelta = (int16_t)v.w[kVolEnv + 1];
	for (int i = 0; i < kSpf; i++)
	{
		smp[i] = (int16_t)clamp16(((int32_t)smp[i] * env) >> 15);
		env = (uint16_t)(env + envDelta);
	}
	v.w[kVolEnv] = env;
	// the one-pole low-pass filter
	if (v.w[kLpf])
	{
		int16_t y = (int16_t)v.w[kLpf + 1];
		const int32_t a0 = v.w[kLpf + 2], b0 = v.w[kLpf + 3];
		for (int i = 0; i < kSpf; i++)
			y = smp[i] = (int16_t)((a0 * smp[i] + b0 * y) >> 15);
		v.w[kLpf + 1] = (uint16_t)y;
	}
	// into the buses the mixer control picks (AXWii's bits: main L/R/ramp/S/S ramp at 0-4, AUX A at
	// 16-20, B at 21-25, C at 26-30); the PB's mixer: volume and delta per bus in this order
	const uint32_t mc = v.at(kMixCtrlHi);
	struct Bus { uint32_t on, ramp; int buf, vol, dpop; };
	static const Bus buses[12] = {
		{1u << 0, 1u << 2, ML, 0, 0}, {1u << 1, 1u << 2, MR, 2, 4}, {1u << 3, 1u << 4, MS, 16, 8},
		{1u << 16, 1u << 18, AL, 4, 1}, {1u << 17, 1u << 18, AR, 6, 5}, {1u << 19, 1u << 20, AS, 18, 9},
		{1u << 21, 1u << 23, BL, 8, 2}, {1u << 22, 1u << 23, BR, 10, 6}, {1u << 24, 1u << 25, BS, 20, 10},
		{1u << 26, 1u << 28, CL, 12, 3}, {1u << 27, 1u << 28, CR, 14, 7}, {1u << 29, 1u << 30, CS, 22, 11}};
	for (const Bus& b : buses)
		if (mc & b.on)
			mixAdd(s_mix[b.buf], smp, v, kMixer + b.vol, kDpop + b.dpop, (mc & b.ramp) != 0);
}

void axProcess(uint32_t pb)
{
	s_muted = ssa::setting("audio") == 1;
	for (int n = 0; pb && n < 1024; n++)
	{
		uint8_t* p = ram(pb, kPbWords * 2);
		if (!p)
			return;
		// (a voice that isn't running - most of the list - mixes nothing and changes nothing in its block:
		// only its link is needed. Copying all 160 words in and out of every block was ~4% of the PS4's
		// game thread in v00.16.)
		if (!(p[2 * kRunning] | p[2 * kRunning + 1]))
		{
			pb = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
			continue;
		}
		Voice v;
		for (int i = 0; i < kPbWords; i++)
			v.w[i] = (uint16_t)(p[2 * i] << 8 | p[2 * i + 1]);
		processVoice(v);
		for (int i = 0; i < kPbWords; i++)
			put16(p + 2 * i, (int16_t)v.w[i]);
		pb = v.at(kNextHi);
	}
}

// the Wii Remote speakers' outputs: silence (18 samples each)
void axWmOutput(const uint32_t* addr)
{
	for (int r = 0; r < 4; r++)
		if (uint8_t* p = ram(addr[r], 18 * 2))
			memset(p, 0, 18 * 2);
}

// the AXWii command list, carried out
void axCommandList(uint32_t address, uint16_t size)
{
	uint32_t i = 0;
	const uint32_t n = size;
	auto next = [&]() -> uint16_t { return i < n ? rd16(address + 2 * i++) : (uint16_t)(s_oldAxWii ? 0x0F : 0x0E); };
	auto next32 = [&]() -> uint32_t {
		const uint32_t hi = next();
		return hi << 16 | next();
	};
	if (!s_oldAxWii && s_mixOn)
	{
		static int listLog = getenv("SSA_DSP_VOICELOG") ? 4 : 0;
		if (listLog > 0)
		{
			listLog--;
			char line[400];
			int o = 0;
			for (uint32_t k = 0; k < n && k < 64 && o < 380; k++)
				o += snprintf(line + o, sizeof(line) - o, "%04x ", rd16(address + 2 * k));
			log("dsp: list at %08x: %s", address, line);
		}
		for (;;)
		{
			const uint16_t cmd = next();
			if (i > n)
				return;
			switch (cmd)
			{
			case 0x00: axSetup(next32()); break;
			case 0x01: axAddToLR(next32(), false); break;
			case 0x02: axAddToLR(next32(), true); break;
			case 0x03: axAddSubToLR(next32()); break;
			case 0x04: axProcess(next32()); break;
			case 0x05: case 0x06: case 0x07:
			{
				const uint16_t volume = next();
				const uint32_t w = next32(), r = next32();
				axMixAux(cmd - 0x05, w, r, volume);
				break;
			}
			case 0x08: case 0x09:
			{
				const uint16_t volume = next();
				uint32_t a[6];
				for (uint32_t& x : a)
					x = next32();
				axUploadAuxMixLRSC(cmd - 0x08, a, volume);
				break;
			}
			case 0x0A: i += 4; break; // compressor
			case 0x0B: case 0x0C:
			{
				// (the surround buffer's address comes first: the game adds it to L/R in the next
				// frame's list, CMD_ADD_TO_LR)
				const uint16_t volume = next();
				const uint32_t s = next32(), lr = next32();
				axOutput(lr, s, volume);
				break;
			}
			case 0x0D:
			{
				uint32_t a[4];
				for (uint32_t& x : a)
					x = next32();
				axWmOutput(a);
				break;
			}
			case 0x0E: return;
			default:
				log("dsp: unknown AXWii command %04x in the list at %08x", cmd, address);
				return;
			}
		}
	}
	// (the older command set, or SSA_AUDIO=0: walked only, voices not mixed)
	for (;;)
	{
		const uint16_t cmd = next();
		if (i > n)
			break;
		if (!s_oldAxWii)
		{
			switch (cmd)
			{
			case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: i += 2; break; // setup, add/sub to LR, process
			case 0x05: case 0x06: case 0x07: i += 5; break;                       // mix AUX A/B/C
			case 0x08: case 0x09: i += 13; break;                                 // upload AUX + mix LRSC
			case 0x0A: i += 4; break;                                             // compressor
			case 0x0B: case 0x0C: i += 5; break;                                  // output (volume, 2 addresses)
			case 0x0D: i += 8; break;                                             // Wii Remote output
			case 0x0E: return;                                                    // end
			default:
				log("dsp: unknown AXWii command %04x in the list at %08x", cmd, address);
				return;
			}
		}
		else
		{
			switch (cmd)
			{
			case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: i += 2; break;
			case 0x05: break;                                                     // process (old)
			case 0x06: case 0x07: case 0x08: i += 5; break;
			case 0x09: case 0x0A: i += 13; break;
			case 0x0B: i += 4; break;
			case 0x0C: case 0x0D: i += 4; break;                                  // output (no volume)
			case 0x0E: i += 8; break;
			case 0x0F: return;
			default:
				log("dsp: unknown old AXWii command %04x in the list at %08x", cmd, address);
				return;
			}
		}
	}
}

void prepareBootUCode(uint32_t mail)
{
	switch (s_uploadSteps)
	{
	case 3: s_uploadIramAddr = mail; break;
	case 4: s_uploadIramSize = mail & 0xFFFF; break;
	default: break;
	}
	if (++s_uploadSteps == 10)
	{
		s_uploadSteps = 0;
		s_uploadSetup = false;
		s_needsResumeMail = true;
		bootByHash(s_uploadIramAddr, s_uploadIramSize, "new");
	}
}

void axMail(uint32_t mail)
{
	if (s_uploadSetup)
	{
		prepareBootUCode(mail);
		return;
	}
	switch (s_axState)
	{
	case AxState::WaitingForCmdListSize:
		if ((mail & 0xFFFF0000) == 0xBABE0000)
		{
			s_cmdListSize = (uint16_t)mail;
			s_axState = AxState::WaitingForCmdListAddress;
		}
		else
			log("dsp: AX expected a command list, got %08x", mail);
		break;
	case AxState::WaitingForCmdListAddress:
		axCommandList(mail, s_cmdListSize);
		if (s_axFrames++ < 3)
			log("dsp: AX command list at %08x (%u words)", mail, s_cmdListSize);
		s_cmdListSize = 0;
		// DSP_YIELD, its interrupt a little later (an empty list's ~2500 DSP cycles)
		pushMail(0xDCD10002, true, 200);
		s_axState = AxState::WaitingForNextTask;
		break;
	case AxState::WaitingForNextTask:
		switch (0xCDD10000 | (mail & 0xFFFF))
		{
		case 0xCDD10000: // resume
			pushMail(0xDCD10001, true);
			s_axState = AxState::WaitingForCmdListSize;
			break;
		case 0xCDD10001: // new microcode
			s_uploadSetup = true;
			s_axState = AxState::WaitingForCmdListSize;
			break;
		case 0xCDD10002: // reset
			setUCode(UCode::Rom);
			break;
		case 0xCDD10003: // continue
			s_axState = AxState::WaitingForCmdListSize;
			break;
		default:
			log("dsp: unknown AX task mail %08x", mail);
			break;
		}
		break;
	}
}

void mailToDsp(uint32_t mail)
{
	if (s_mailLogs > 0)
	{
		s_mailLogs--;
		log("dsp: mail from the CPU %08x", mail);
	}
	switch (s_ucode)
	{
	case UCode::Rom: romMail(mail); break;
	case UCode::AXWii: axMail(mail); break;
	default: break;
	}
}

// ---- audio DMA ----
uint32_t s_dmaStart = 0;        // the address of the buffer to play next
uint16_t s_dmaControl = 0;      // bit 15 start, bits 14-0 length in 32-byte blocks
bool s_dmaPlaying = false;
uint64_t s_dmaEnd = 0;          // when the playing buffer ends
uint32_t s_dmaBlocks = 0;       // its length
uint64_t s_dmaBuffers = 0;
uint64_t s_initCodeClear = 0;   // DSPInitCode reads clear from this time base on

// a started buffer's samples to the host (the PS4's audio port; SSA_AUDIO_WAV=file on the PC):
// big-endian right/left pairs at 32 kHz -> host-order left/right
void sendToHost(uint32_t start, uint32_t blocks)
{
	static FILE* wav = [] {
		const char* path = getenv("SSA_AUDIO_WAV");
		FILE* f = path ? fopen(path, "wb") : nullptr;
		if (f)
		{
			static const uint8_t header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x00, 0x7D, 0, 0,
				0x00, 0xF4, 0x01, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
			fwrite(header, 1, 44, f);
		}
		return f;
	}();
	static uint32_t wavBytes = 0;
	if ((!g_audioSink && !wav) || !blocks)
		return;
	const uint32_t frames = blocks * 8;
	const uint8_t* p = ram(start, frames * 4);
	if (!p)
		return;
	static std::vector<int16_t> lr;
	lr.resize(frames * 2);
	for (uint32_t i = 0; i < frames; i++)
	{
		lr[2 * i] = (int16_t)(p[4 * i + 2] << 8 | p[4 * i + 3]);
		lr[2 * i + 1] = (int16_t)(p[4 * i] << 8 | p[4 * i + 1]);
	}
	if (g_audioSink && !s_muted)
		g_audioSink(lr.data(), frames);
	if (wav)
	{
		fwrite(lr.data(), 4, frames, wav);
		wavBytes += frames * 4;
		const uint32_t riff = wavBytes + 36;
		fseek(wav, 4, SEEK_SET);
		fwrite(&riff, 4, 1, wav);
		fseek(wav, 40, SEEK_SET);
		fwrite(&wavBytes, 4, 1, wav);
		fseek(wav, 0, SEEK_END);
	}
}

// a buffer starts: now (the CPU started the DMA), or where the last one ended (the next one, chained:
// the time base's own 32 kHz, so the game's sound comes at the real rate however late the poll that
// saw the end was; more than 30 ms behind, it starts again from now)
void startDmaBuffer(uint64_t from = 0)
{
	s_dmaBlocks = s_dmaControl & 0x7FFF;
	sendToHost(s_dmaStart, s_dmaBlocks);
	// 32 bytes = 8 stereo 16-bit samples; 32 kHz
	const uint64_t samples = (uint64_t)s_dmaBlocks * 8, t = now();
	if (!from || t - from > 60750000 / 33)
		from = t;
	s_dmaEnd = from + samples * 60750000 / 32000;
	s_dmaPlaying = s_dmaBlocks != 0;
	s_dmaBuffers++;
}
} // namespace

// ---- called by mmio.cpp ----
bool dspOwns(uint32_t a)
{
	return (a >= 0xCC005000 && a < 0xCC005008) || (a >= 0xCC005030 && a < 0xCC00503C);
}

uint32_t dspRead(uint32_t a, int size)
{
	auto half = [&](uint32_t at) -> uint16_t {
		switch (at)
		{
		case 0xCC005000: return (uint16_t)(s_cpuMailbox >> 16);
		case 0xCC005002: return (uint16_t)s_cpuMailbox;
		case 0xCC005004:
			if (!s_halted && !s_toCpu.empty())
				s_lastMail = s_toCpu.front().first;
			return (uint16_t)(s_lastMail >> 16);
		case 0xCC005006:
		{
			if (!s_halted && !s_toCpu.empty())
			{
				s_lastMail = s_toCpu.front().first;
				const bool interrupt = s_toCpu.front().second;
				s_toCpu.pop_front();
				if (interrupt)
					dspInterrupt();
			}
			const uint16_t lo = (uint16_t)s_lastMail;
			s_lastMail &= ~0x80000000u;
			return lo;
		}
		case 0xCC005030: return (uint16_t)(s_dmaStart >> 16);
		case 0xCC005032: return (uint16_t)s_dmaStart;
		case 0xCC005036: return s_dmaControl;
		case 0xCC00503A:
		{
			// blocks left in the playing buffer
			if (!s_dmaPlaying || !s_dmaBlocks)
				return 0;
			const uint64_t t = now();
			if (t >= s_dmaEnd)
				return 0;
			const uint64_t total = (uint64_t)s_dmaBlocks * 8 * 60750000 / 32000;
			return (uint16_t)((s_dmaEnd - t) * s_dmaBlocks / (total ? total : 1));
		}
		}
		return 0;
	};
	if (size == 4)
		return (uint32_t)half(a) << 16 | half(a + 2);
	if (size == 2)
		return half(a & ~1u);
	const uint16_t h = half(a & ~1u);
	return (a & 1) ? (h & 0xFF) : (h >> 8);
}

void dspWrite(uint32_t a, uint32_t value, int size)
{
	auto half = [&](uint32_t at, uint16_t v) {
		switch (at)
		{
		case 0xCC005000: s_cpuMailbox = (s_cpuMailbox & 0xFFFF) | (uint32_t)v << 16; break;
		case 0xCC005002:
			s_cpuMailbox = (s_cpuMailbox & 0xFFFF0000u) | v;
			mailToDsp(s_cpuMailbox);
			s_cpuMailbox &= 0x7FFFFFFF; // taken
			break;
		case 0xCC005030: s_dmaStart = (s_dmaStart & 0xFFFF) | (uint32_t)(v & 0x1FFF) << 16; break;
		case 0xCC005032: s_dmaStart = (s_dmaStart & 0xFFFF0000u) | (v & 0xFFE0); break;
		case 0xCC005036:
		{
			const bool wasOn = s_dmaControl & 0x8000;
			s_dmaControl = v;
			if ((v & 0x8000) && !wasOn)
				startDmaBuffer();
			if (!(v & 0x8000))
				s_dmaPlaying = false;
			break;
		}
		default: break;
		}
	};
	if (size == 4)
	{
		half(a, (uint16_t)(value >> 16));
		half(a + 2, (uint16_t)value);
	}
	else if (size == 2)
		half(a & ~1u, (uint16_t)value);
}

// CSR written: halt, reset, DSPInit (bit 11, cleared: the INIT microcode). Returns the CSR to keep.
uint32_t dspControlWritten(uint32_t old, uint32_t csr)
{
	s_halted = csr & (1u << 2);
	if (csr & 1) // reset
	{
		setUCode(UCode::Rom);
		csr &= ~1u;
	}
	if ((old & (1u << 11)) && !(csr & (1u << 11)))
	{
		setUCode(UCode::Init);
		csr |= 1u << 10; // DSPInitCode, for a moment
		s_initCodeClear = now() + 130;
	}
	return csr;
}

// CSR read: DSPInitCode clears after its moment
uint32_t dspControlRead(uint32_t csr)
{
	if ((csr & (1u << 10)) && now() >= s_initCodeClear)
		csr &= ~(1u << 10);
	return csr;
}

// called by the runtime regularly: delayed DSP interrupts, the microcode's resume mail, and the
// audio DMA's buffer ends
void dspUpdate(uint64_t timeBase)
{
	// every 10 s: the audio's progress
	static uint64_t lastReport = 0, lastFrames = 0, lastVoices = 0, lastBuffers = 0;
	if (timeBase - lastReport >= 607500000ull)
	{
		if (lastReport)
			log("dsp: 10 s: %llu AX frames, %llu voice frames mixed, %llu audio DMA buffers", (unsigned long long)(s_axFrames - lastFrames),
				(unsigned long long)(s_voicesMixed - lastVoices), (unsigned long long)(s_dmaBuffers - lastBuffers));
		lastReport = timeBase;
		lastFrames = s_axFrames;
		lastVoices = s_voicesMixed;
		lastBuffers = s_dmaBuffers;
	}
	for (size_t i = 0; i < s_delayedInterrupts.size();)
		if (timeBase >= s_delayedInterrupts[i].first)
		{
			dspInterrupt();
			s_delayedInterrupts.erase(s_delayedInterrupts.begin() + i);
		}
		else
			i++;
	if (s_ucode == UCode::AXWii && s_needsResumeMail)
	{
		s_needsResumeMail = false;
		pushMail(0xDCD10001, true);
	}
	if (s_dmaPlaying && timeBase >= s_dmaEnd)
	{
		mmioCsrSet(1u << 3); // AIDINT: the buffer has played
		if (s_dmaControl & 0x8000)
			startDmaBuffer(s_dmaEnd); // the next one (the address and length the CPU set meanwhile)
		else
			s_dmaPlaying = false;
	}
}

// SSPE52's AXFX high-quality reverb: the RVL SDK's HandleReverb (guest 0x8037AA70), which the AX frame
// callback runs once per 96-sample frame (3 ms) per reverb with the frame's L/R/S buffers (s32) and
// its work area. As translated code it was ~8% of the PS4's game thread (v00.17 samples: delay lines of
// floats, lfsx/stfsx, every single-precision op rounded through doubles). Here: the same
// single-precision operations in the same order on the same state (delay lines, positions, the
// low-pass filter's last samples, all where the game keeps them). The work area (offsets from rv):
//   0x00 early-reflection lines [3], 0x0C/10/14 their 3 taps' positions, 0x18 their length,
//   0x20/24/28 the taps' coefficients; 0x2C pre-delay lines [3], 0x38 position, 0x3C length (0: none);
//   0x44 comb lines [3 channels][3], 0x68 positions [3], 0x74 lengths [3], 0x8C coefficients [3];
//   0x98 all-pass lines [3][2], 0xB0 positions [2], 0xB8 lengths [2]; 0xC8 the last all-pass's lines
//   [3], 0xD4 its positions [3], 0xE0 lengths [3]; 0xF8 the all-pass coefficient; 0xFC the low-pass
//   filter's last samples [3]; 0x108 damping; 0x10C "skip once" (bit 1); 0x12C crosstalk; 0x134 the
//   reverb's level; 0x138 / 0x13C the aux bus in / out (3 buffer pointers each, or 0); 0x140 / 0x144
//   the gains to the main buffers / the bus.
// False: something isn't where it should be (nothing done; the caller runs the guest's code).
bool axfxReverbHiNative(uint32_t bufs, uint32_t rv)
{
	auto u32 = [](uint32_t a, bool& ok) -> uint32_t {
		const uint8_t* q = ram(a, 4);
		if (!q)
		{
			ok = false;
			return 0;
		}
		return (uint32_t)ram32(q);
	};
	auto f32 = [](const uint8_t* q) {
		const uint32_t v = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
		float f;
		memcpy(&f, &v, 4);
		return f;
	};
	auto stf = [](uint8_t* q, float f) {
		uint32_t v;
		memcpy(&v, &f, 4);
		q[0] = (uint8_t)(v >> 24);
		q[1] = (uint8_t)(v >> 16);
		q[2] = (uint8_t)(v >> 8);
		q[3] = (uint8_t)v;
	};
	// fctiwz: toward zero, saturating (NaN: 0x80000000)
	auto toInt = [](float f) -> int32_t {
		if (!(f > -2147483648.0f))
			return INT32_MIN;
		if (f >= 2147483648.0f)
			return INT32_MAX;
		return (int32_t)f;
	};
	uint8_t* w = ram(rv, 0x148);
	const uint8_t* b = ram(bufs, 12);
	if (!w || !b)
		return false;
	bool ok = true;
	if (const uint32_t skip = (uint32_t)ram32(w + 0x10C))
	{
		put32(w + 0x10C, (int32_t)(skip & ~2u));
		return true;
	}
	constexpr int kFrame = 96;
	// the buffers: main L/R/S (read, then the reverb's output written over them), the bus in / out
	uint8_t *mainBuf[3], *inBuf[3] = {}, *outBuf[3] = {};
	for (int c = 0; c < 3; c++)
		if (!(mainBuf[c] = ram((uint32_t)ram32(b + 4 * c), 4 * kFrame)))
			return false;
	const uint32_t busIn = (uint32_t)ram32(w + 0x138), busOut = (uint32_t)ram32(w + 0x13C);
	for (int c = 0; c < 3; c++)
	{
		if (busIn && !(inBuf[c] = ram(u32(busIn + 4 * c, ok), 4 * kFrame)))
			return false;
		if (busOut && !(outBuf[c] = ram(u32(busOut + 4 * c, ok), 4 * kFrame)))
			return false;
	}
	if (!ok)
		return false;
	// the lines (each long enough for its positions)
	const uint32_t earlyLen = (uint32_t)ram32(w + 0x18), preLen = (uint32_t)ram32(w + 0x3C);
	uint32_t combLen[3], apLen[2], lastLen[3];
	for (int k = 0; k < 3; k++)
		combLen[k] = (uint32_t)ram32(w + 0x74 + 4 * k);
	for (int j = 0; j < 2; j++)
		apLen[j] = (uint32_t)ram32(w + 0xB8 + 4 * j);
	for (int c = 0; c < 3; c++)
		lastLen[c] = (uint32_t)ram32(w + 0xE0 + 4 * c);
	auto line = [&](uint32_t at, uint32_t length) -> uint8_t* { return length ? ram((uint32_t)ram32(w + at), 4 * length) : nullptr; };
	uint8_t *early[3], *pre[3] = {}, *comb[3][3], *ap[3][2], *last[3];
	for (int c = 0; c < 3; c++)
	{
		if (!(early[c] = line(0x00 + 4 * c, earlyLen)) || (preLen && !(pre[c] = line(0x2C + 4 * c, preLen))) ||
			!(last[c] = line(0xC8 + 4 * c, lastLen[c])))
			return false;
		for (int k = 0; k < 3; k++)
			if (!(comb[c][k] = line(0x44 + 12 * c + 4 * k, combLen[k])))
				return false;
		for (int j = 0; j < 2; j++)
			if (!(ap[c][j] = line(0x98 + 8 * c + 4 * j, apLen[j])))
				return false;
	}
	uint32_t ep[3], cp[3], app[2], lp[3], pp = (uint32_t)ram32(w + 0x38);
	for (int k = 0; k < 3; k++)
	{
		ep[k] = (uint32_t)ram32(w + 0x0C + 4 * k);
		cp[k] = (uint32_t)ram32(w + 0x68 + 4 * k);
		lp[k] = (uint32_t)ram32(w + 0xD4 + 4 * k);
		if (ep[k] >= earlyLen || cp[k] >= combLen[k] || lp[k] >= lastLen[k])
			return false;
	}
	for (int j = 0; j < 2; j++)
		if ((app[j] = (uint32_t)ram32(w + 0xB0 + 4 * j)) >= apLen[j])
			return false;
	if (preLen && pp >= preLen)
		return false;
	// the constants (.sdata2: 0, 1, the level's and the crosstalk's scales)
	const uint8_t* k = ram(0x806790C4, 16);
	if (!k)
		return false;
	const float kZero = f32(k), kOne = f32(k + 4), kLevel = f32(k + 8), kCross = f32(k + 12);
	const float damping = f32(w + 0x108), undamped = kOne - damping, level = kLevel * f32(w + 0x134), g = f32(w + 0xF8),
				cross = kCross * f32(w + 0x12C);
	const float ec[3] = {f32(w + 0x20), f32(w + 0x24), f32(w + 0x28)}, cc[3] = {f32(w + 0x8C), f32(w + 0x90), f32(w + 0x94)};
	for (int s = 0; s < kFrame; s++)
	{
		float o[3];
		for (int c = 0; c < 3; c++)
		{
			int32_t x = ram32(mainBuf[c] + 4 * s);
			if (busIn)
				x = (int32_t)((uint32_t)x + (uint32_t)ram32(inBuf[c] + 4 * s));
			const float in = (float)x;
			// early reflections: three taps, the newest sample written over the third
			const float t0 = f32(early[c] + 4 * ep[0]), t1 = f32(early[c] + 4 * ep[1]), t2 = f32(early[c] + 4 * ep[2]);
			stf(early[c] + 4 * ep[2], in);
			const float e0 = ec[0] * t0, e1 = ec[1] * t1, e2 = ec[2] * t2;
			const float er = e2 + (e0 + e1);
			// the pre-delay
			float p = in;
			if (preLen)
			{
				p = f32(pre[c] + 4 * pp);
				stf(pre[c] + 4 * pp, in);
			}
			// three combs in parallel (their delayed samples summed)
			float sum = kZero;
			for (int n = 0; n < 3; n++)
			{
				uint8_t* q = comb[c][n] + 4 * cp[n];
				const float t = f32(q), fb = t * cc[n];
				sum = sum + t;
				stf(q, p + fb);
			}
			// two all-passes in series
			float y = sum;
			for (int j = 0; j < 2; j++)
			{
				uint8_t* q = ap[c][j] + 4 * app[j];
				const float t = f32(q), v = y + t * g;
				stf(q, v);
				const float vg = v * g;
				y = t - vg;
			}
			// the low-pass filter
			{
				uint8_t* q = w + 0xFC + 4 * c;
				const float a = undamped * y, d = damping * f32(q);
				y = a + d;
				stf(q, y);
			}
			// the channel's last all-pass, then the level, with the early reflections
			{
				uint8_t* q = last[c] + 4 * lp[c];
				const float t = f32(q), v = y + t * g;
				stf(q, v);
				const float vg = v * g;
				if (++lp[c] >= lastLen[c])
					lp[c] = 0;
				const float r = t - vg, rl = r * level;
				o[c] = rl + er;
			}
		}
		// the channels with each other's crosstalk, to the main buffers (and the bus)
		const float rs = o[1] + o[2], ls = o[0] + o[2], lr = o[0] + o[1];
		const float rsx = rs * cross, lsx = ls * cross, lrx = lr * cross;
		const float out[3] = {o[0] + rsx, o[1] + lsx, o[2] + lrx};
		const float gain = f32(w + 0x140);
		for (int c = 0; c < 3; c++)
			put32(mainBuf[c] + 4 * s, toInt(out[c] * gain));
		if (busOut)
		{
			const float busGain = f32(w + 0x144);
			for (int c = 0; c < 3; c++)
				put32(outBuf[c] + 4 * s, toInt(out[c] * busGain));
		}
		for (int n = 0; n < 3; n++)
		{
			if (++ep[n] >= earlyLen)
				ep[n] = 0;
			if (++cp[n] >= combLen[n])
				cp[n] = 0;
		}
		if (preLen && ++pp >= preLen)
			pp = 0;
		for (int j = 0; j < 2; j++)
			if (++app[j] >= apLen[j])
				app[j] = 0;
	}
	for (int n = 0; n < 3; n++)
	{
		put32(w + 0x0C + 4 * n, (int32_t)ep[n]);
		put32(w + 0x68 + 4 * n, (int32_t)cp[n]);
		put32(w + 0xD4 + 4 * n, (int32_t)lp[n]);
	}
	for (int j = 0; j < 2; j++)
		put32(w + 0xB0 + 4 * j, (int32_t)app[j]);
	if (preLen)
		put32(w + 0x38, (int32_t)pp);
	return true;
}

// the guest memory the reverb reads and writes (guest address, bytes), for SSA_REVERB_CHECK
std::vector<std::pair<uint32_t, uint32_t>> axfxReverbHiRegions(uint32_t bufs, uint32_t rv)
{
	std::vector<std::pair<uint32_t, uint32_t>> r;
	auto rd = [](uint32_t a) -> uint32_t {
		const uint8_t* q = ram(a, 4);
		return q ? (uint32_t)ram32(q) : 0;
	};
	r.push_back({rv, 0x148});
	for (int c = 0; c < 3; c++)
	{
		r.push_back({rd(bufs + 4 * c), 4 * 96});
		if (const uint32_t in = rd(rv + 0x138))
			r.push_back({rd(in + 4 * c), 4 * 96});
		if (const uint32_t out = rd(rv + 0x13C))
			r.push_back({rd(out + 4 * c), 4 * 96});
		r.push_back({rd(rv + 4 * c), 4 * rd(rv + 0x18)});
		if (rd(rv + 0x3C))
			r.push_back({rd(rv + 0x2C + 4 * c), 4 * rd(rv + 0x3C)});
		for (int k = 0; k < 3; k++)
			r.push_back({rd(rv + 0x44 + 12 * c + 4 * k), 4 * rd(rv + 0x74 + 4 * k)});
		for (int j = 0; j < 2; j++)
			r.push_back({rd(rv + 0x98 + 8 * c + 4 * j), 4 * rd(rv + 0xB8 + 4 * j)});
		r.push_back({rd(rv + 0xC8 + 4 * c), 4 * rd(rv + 0xE0 + 4 * c)});
	}
	return r;
}

uint64_t dspAxFrames() { return s_axFrames; }
uint64_t dspDmaBuffers() { return s_dmaBuffers; }
