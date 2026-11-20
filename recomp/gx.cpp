// Skylanders runtime: the GX command stream (the GPU's FIFO), decoded. After Dolphin's VideoCommon
// (OpcodeDecoding, VertexLoader, XF/BP/CP state, VertexShaderGen/LightingShaderGen, TextureDecoder,
// TMEM; GPL-2.0-or-later, like the rest of the runtime).
//
// The write-gather pipe's bytes (mmio.cpp) come here as they reach the FIFO. The decoder keeps the
// GPU's register state (CP: vertex formats and arrays; XF: matrices, lights, texgens, color
// channels, viewport, projection; BP: textures, TEV, blending, depth) and does the transform
// stage on the CPU: positions to clip space, normals, the two lit color channels and all texgens
// (texture matrices, post-transform matrices). Vertices and the draw's triangles go to the Vulkan
// backend (gx_vk.cpp), which does the rest: clipping, rasterizing, the TEV, blending, depth and
// EFB copies. Display lists (CALL_DL) are decoded from memory where they are called; indexed XF
// loads read their arrays; TLUT loads go to a TMEM model.
//
// Frames are counted at XFB copies. On the PC host only captured frames are drawn (SSA_FRAMES=dir,
// every SSA_FRAMES_EVERY-th frame and the two before it, as PNGs), or every frame with SSA_VK_ALL=1.
// Not yet: lines and points, emboss offsets, mipmaps.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <zlib.h>
#include <atomic>
#include <deque>
#include <thread>
#include <pthread.h>
#include <signal.h>

#include "gx_vk.h"

extern uint8_t* memory_base;
extern std::atomic<int> g_ssaPaused; // overlay.cpp: the quick menu is open, the game thread waits
// the host's thread placement (ps4/ps4_threads.cpp: a core of its own and a priority; the PC: none)
void (*g_ssaPinThread)(int core, int priority, const char* name) = nullptr;
uint64_t g_gxTicksTextures = 0;
uint32_t g_gxStateVersion = 0;

namespace
{
// time-stamp ticks to milliseconds (the rate measured against the clock since the start)
const uint64_t s_tsc0 = __builtin_ia32_rdtsc();
const auto s_clock0 = std::chrono::steady_clock::now();
double tscPerNs()
{
	const double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s_clock0).count();
	return ns > 1e6 ? (double)(__builtin_ia32_rdtsc() - s_tsc0) / ns : 1.6;
}
double tscMs(uint64_t ticks)
{
	return ticks / tscPerNs() / 1e6;
}

// ---- state ----
// the GX thread (below): on (1), off (0), not decided yet (-1); the pixel engine's events it decoded
int s_threadMode = -1;
std::atomic<uint32_t> s_peDone{0}, s_peTokenInts{0}, s_peTokenSeq{0}, s_peToken{0};
uint32_t s_cp[256];
bool s_xfDirty = true; // (XF memory, MATINDEX or the scissor offset changed since the last XF snapshot)
uint32_t s_xf[0x1100];  // 0x0000-0x0FFF: XF memory (matrices, lights); 0x1000-0x10FF: registers
uint32_t s_bp[256];
int32_t s_tevColor[4][4], s_tevKonst[4][4]; // BP 0xE0-0xE7: the color and konst registers share addresses
std::vector<uint8_t> s_tmem(0x100000); // texture memory (TLUTs here; textures are read from RAM)
std::vector<uint8_t> s_pending;        // FIFO bytes not yet forming a whole command

struct Stats
{
	uint64_t commands = 0, draws = 0, vertices = 0, triangles = 0, displayLists = 0, xfLoads = 0, bpWrites = 0, cpWrites = 0;
	uint64_t frames = 0, unknown = 0, lines = 0;
	uint64_t posSkip = 0, posNull = 0, pnmtxVerts = 0, pnmtxOdd = 0;
	uint64_t vertexNs = 0; // (time-stamp ticks in the vertex loops: fetch, or the CPU transform; tscMs() for ms)
} s_stats, s_frameStats;

uint32_t be32(const uint8_t* p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
uint16_t be16(const uint8_t* p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}
float asFloat(uint32_t v)
{
	float f;
	memcpy(&f, &v, 4);
	return f;
}
uint32_t asBits(float f)
{
	uint32_t v;
	memcpy(&v, &f, 4);
	return v;
}
// guest physical address (MEM1 at 0, MEM2 at 0x10000000) -> host, with the bytes available there
const uint8_t* phys(uint32_t a, uint32_t* avail = nullptr)
{
	a &= 0x1FFFFFFF;
	if (a < 0x01800000)
	{
		if (avail)
			*avail = 0x01800000 - a;
		return memory_base + 0x80000000u + a;
	}
	if (a >= 0x10000000 && a < 0x14000000)
	{
		if (avail)
			*avail = 0x14000000 - a;
		return memory_base + 0x80000000u + a;
	}
	return nullptr;
}

// ---- the frame recorder (SSA_GXREC=file, after Dolphin's FIFO player) ----
// From the first frame with SSA_GXREC_DRAWS draws (default 20000) on, SSA_GXREC_FRAMES frames
// (default 3) are recorded: the GX state and all of RAM, then the FIFO bytes as they come, each
// preceded by the RAM ranges the decoder read that changed since. gx_replay draws them again.
// Records: 1 FIFO bytes (u32 length, bytes), 2 RAM (u32 physical address, u32 length, bytes), 0 end.
gzFile s_rec = nullptr;
bool s_unrecordedClear = false; // (bytes from before the recording are in the state)
std::vector<uint8_t> s_shadow;  // RAM as the recording has it
std::vector<uint8_t> s_patches; // RAM records found while decoding the current burst
enum RecState
{
	kRecOff,
	kRecArmed,
	kRecStart,
	kRecOn,
	kRecStop
} s_recState = kRecOff;
int s_recFrames = 0, s_recWanted = 3;
uint64_t s_recDraws = 20000;
uint64_t s_recAt = 0; // SSA_GXREC_AT: not before this frame (XFB copy count)
uint64_t s_recMaxDraws = ~0ull; // SSA_GXREC_MAXDRAWS: the first frame recorded has at most this many draws
constexpr uint32_t kMem1 = 0x01800000, kMem2 = 0x04000000;

uint32_t shadowOffset(uint32_t a)
{
	a &= 0x1FFFFFFF;
	return a < kMem1 ? a : a - 0x10000000 + kMem1;
}

void putBytes(std::vector<uint8_t>& v, const void* p, size_t n)
{
	v.insert(v.end(), (const uint8_t*)p, (const uint8_t*)p + n);
}

// RAM the decoder reads (len bytes from a); while recording, a changed range becomes a record
const uint8_t* readRam(uint32_t a, uint32_t len, uint32_t* avail = nullptr)
{
	uint32_t av = 0;
	const uint8_t* p = phys(a, &av);
	if (avail)
		*avail = av;
	if (!p || !s_rec || !len)
		return p;
	len = std::min(len, av);
	uint8_t* sh = &s_shadow[shadowOffset(a)];
	if (memcmp(sh, p, len))
	{
		uint32_t lo = 0, hi = len;
		while (sh[lo] == p[lo])
			lo++;
		while (sh[hi - 1] == p[hi - 1])
			hi--;
		const uint8_t type = 2;
		const uint32_t addr = (a & 0x1FFFFFFF) + lo, n = hi - lo;
		putBytes(s_patches, &type, 1);
		putBytes(s_patches, &addr, 4);
		putBytes(s_patches, &n, 4);
		putBytes(s_patches, p + lo, n);
		memcpy(sh + lo, p + lo, n);
	}
	return p;
}

void recStart()
{
	const char* path = getenv("SSA_GXREC");
	s_rec = gzopen(path, "wb1");
	if (!s_rec)
	{
		s_recState = kRecOff;
		return;
	}
	gzwrite(s_rec, "GXR1", 4);
	gzwrite(s_rec, s_cp, sizeof(s_cp));
	gzwrite(s_rec, s_xf, sizeof(s_xf));
	gzwrite(s_rec, s_bp, sizeof(s_bp));
	gzwrite(s_rec, s_tevColor, sizeof(s_tevColor));
	gzwrite(s_rec, s_tevKonst, sizeof(s_tevKonst));
	gzwrite(s_rec, s_tmem.data(), (unsigned)s_tmem.size());
	const uint32_t pending = (uint32_t)s_pending.size();
	gzwrite(s_rec, &pending, 4);
	if (pending)
		gzwrite(s_rec, s_pending.data(), pending);
	s_shadow.resize((size_t)kMem1 + kMem2);
	memcpy(s_shadow.data(), memory_base + 0x80000000u, kMem1);
	memcpy(s_shadow.data() + kMem1, memory_base + 0x90000000u, kMem2);
	gzwrite(s_rec, s_shadow.data(), (unsigned)s_shadow.size());
	s_recState = kRecOn;
	s_recFrames = 0;
	s_unrecordedClear = true;
	fprintf(stderr, "gx: recording %d frames to %s\n", s_recWanted, path);
}

void recStop()
{
	const uint8_t end = 0;
	gzwrite(s_rec, &end, 1);
	gzclose(s_rec);
	s_rec = nullptr;
	std::vector<uint8_t>().swap(s_shadow);
	s_recState = kRecOff;
	fprintf(stderr, "gx: recording done\n");
}

// ---- frames ----
std::string s_frameDir;
int s_frameEvery = 30, s_framesWritten = 0, s_frameLimit = 300;
uint64_t s_frameFrom = 0; // SSA_FRAMES_FROM: no captures before this frame
bool s_capturing = false, s_renderAll = false;

void pngChunk(FILE* f, const char* type, const std::vector<uint8_t>& data)
{
	const uint8_t len[4] = {(uint8_t)(data.size() >> 24), (uint8_t)(data.size() >> 16), (uint8_t)(data.size() >> 8), (uint8_t)data.size()};
	fwrite(len, 1, 4, f);
	fwrite(type, 1, 4, f);
	fwrite(data.data(), 1, data.size(), f);
	uLong crc = crc32(0, (const Bytef*)type, 4);
	crc = crc32(crc, data.data(), (uInt)data.size());
	const uint8_t c[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
	fwrite(c, 1, 4, f);
}

// ---- textures: GX formats into ARGB ----
uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
	return a << 24 | r << 16 | g << 8 | b;
}
uint32_t from565(uint16_t v)
{
	return argb(255, (v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31);
}
uint32_t from5A3(uint16_t v)
{
	if (v & 0x8000)
		return argb(255, ((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31);
	return argb(((v >> 12) & 7) * 255 / 7, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17);
}
uint32_t fromIA8(uint16_t v) // (TLUT IA8: alpha in the high byte, intensity in the low)
{
	const uint32_t i = v & 0xFF;
	return argb(v >> 8, i, i, i);
}
uint32_t tlutColor(uint32_t tlutAddr, uint32_t tlutFmt, uint32_t index)
{
	const uint16_t v = be16(&s_tmem[(tlutAddr + index * 2) & 0xFFFFF]);
	return tlutFmt == 0 ? fromIA8(v) : tlutFmt == 1 ? from565(v) : from5A3(v);
}

// tile size and bits per texel of a texture format; false: not a texture format
bool formatTiles(int fmt, int& tw, int& th, int& bpp)
{
	switch (fmt)
	{
	case 0: case 8: tw = 8; th = 8; bpp = 4; return true;          // I4, C4
	case 1: case 2: case 9: tw = 8; th = 4; bpp = 8; return true;  // I8, IA4, C8
	case 3: case 4: case 5: case 10: tw = 4; th = 4; bpp = 16; return true; // IA8, RGB565, RGB5A3, C14X2
	case 6: tw = 4; th = 4; bpp = 32; return true;                 // RGBA8
	case 14: tw = 8; th = 8; bpp = 4; return true;                 // CMPR
	default: return false;
	}
}

// ---- vertex formats (CP 0x50 VCD_LO, 0x60 VCD_HI, 0x70/0x80/0x90 + n VAT A/B/C) ----
int compSize(int fmt)
{
	return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4;
}
const int kColorSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};

struct VtxFormat
{
	int size = 0;
	int pnmtxOff = -1;
	int texMtxOff[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	int posOff = -1, posMode = 0, posFmt = 0, posCount = 2, posShift = 0;
	int nrmOff = -1, nrmMode = 0, nrmFmt = 0, nrmCount = 1;
	bool nrmIndex3 = false;
	int colOff[2] = {-1, -1}, colMode[2] = {}, colFmt[2] = {};
	int texOff[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, texMode[8] = {}, texFmt[8] = {}, texCount[8] = {}, texShift[8] = {};
};

VtxFormat vertexFormat(int vat)
{
	VtxFormat f;
	const uint32_t lo = s_cp[0x50], hi = s_cp[0x60];
	const uint32_t a = s_cp[0x70 + vat], b = s_cp[0x80 + vat], c = s_cp[0x90 + vat];
	int off = 0;
	if (lo & 1)
		f.pnmtxOff = off++;
	for (int t = 0; t < 8; t++)
		if (lo & (2u << t))
			f.texMtxOff[t] = off++;
	auto indexSize = [](int mode) { return mode == 2 ? 1 : 2; };
	// position
	f.posMode = (lo >> 9) & 3;
	f.posCount = (a & 1) ? 3 : 2;
	f.posFmt = (a >> 1) & 7;
	f.posShift = (a >> 4) & 31;
	if (f.posMode)
	{
		f.posOff = off;
		off += f.posMode == 1 ? f.posCount * compSize(f.posFmt) : indexSize(f.posMode);
	}
	// normal (and binormals)
	f.nrmMode = (lo >> 11) & 3;
	if (f.nrmMode)
	{
		const bool nbt = (a >> 9) & 1;
		f.nrmIndex3 = nbt && ((a >> 31) & 1);
		f.nrmFmt = (a >> 10) & 7;
		f.nrmCount = nbt ? 3 : 1;
		f.nrmOff = off;
		if (f.nrmMode == 1)
			off += f.nrmCount * 3 * compSize(f.nrmFmt);
		else
			off += (f.nrmIndex3 ? 3 : 1) * indexSize(f.nrmMode);
	}
	// colors
	for (int n = 0; n < 2; n++)
	{
		const int mode = (lo >> (13 + 2 * n)) & 3;
		if (!mode)
			continue;
		f.colOff[n] = off;
		f.colMode[n] = mode;
		f.colFmt[n] = n == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
		off += mode == 1 ? kColorSize[f.colFmt[n]] : indexSize(mode);
	}
	// texture coordinates
	const int texCnt[8] = {(int)((a >> 21) & 1), (int)(b & 1), (int)((b >> 9) & 1), (int)((b >> 18) & 1), (int)((b >> 27) & 1),
		(int)((c >> 5) & 1), (int)((c >> 14) & 1), (int)((c >> 23) & 1)};
	const int texFmt[8] = {(int)((a >> 22) & 7), (int)((b >> 1) & 7), (int)((b >> 10) & 7), (int)((b >> 19) & 7), (int)((b >> 28) & 7),
		(int)((c >> 6) & 7), (int)((c >> 15) & 7), (int)((c >> 24) & 7)};
	const int texShf[8] = {(int)((a >> 25) & 31), (int)((b >> 4) & 31), (int)((b >> 13) & 31), (int)((b >> 22) & 31), (int)(c & 31),
		(int)((c >> 9) & 31), (int)((c >> 18) & 31), (int)((c >> 27) & 31)};
	for (int t = 0; t < 8; t++)
	{
		const int mode = (hi >> (2 * t)) & 3;
		if (!mode)
			continue;
		f.texOff[t] = off;
		f.texMode[t] = mode;
		f.texFmt[t] = texFmt[t];
		f.texCount[t] = texCnt[t] ? 2 : 1;
		f.texShift[t] = texShf[t];
		off += mode == 1 ? f.texCount[t] * compSize(texFmt[t]) : indexSize(mode);
	}
	f.size = off;
	return f;
}

// vertexFormat(vat), kept per VAT while VCD and that VAT are the same (the decoder needs every draw's
// size, and the draws' fetch plans start from it)
const VtxFormat& cachedFormat(int vat)
{
	struct Entry
	{
		uint32_t key[5] = {~0u, ~0u, ~0u, ~0u, ~0u};
		VtxFormat f;
	};
	static Entry cache[8];
	Entry& e = cache[vat & 7];
	const uint32_t key[5] = {s_cp[0x50], s_cp[0x60], s_cp[0x70 + vat], s_cp[0x80 + vat], s_cp[0x90 + vat]};
	if (memcmp(e.key, key, sizeof(key)))
	{
		memcpy(e.key, key, sizeof(key));
		e.f = vertexFormat(vat);
	}
	return e.f;
}

float readComp(const uint8_t* p, int fmt, int shift)
{
	const float scale = 1.0f / (float)(1u << shift);
	switch (fmt)
	{
	case 0: return p[0] * scale;
	case 1: return (int8_t)p[0] * scale;
	case 2: return be16(p) * scale;
	case 3: return (int16_t)be16(p) * scale;
	default: return asFloat(be32(p));
	}
}

// normals have fixed fractions: u8 7 bits, s8 6, u16 15, s16 14
float readNormalComp(const uint8_t* p, int fmt)
{
	switch (fmt)
	{
	case 0: return p[0] / 128.0f;
	case 1: return (int8_t)p[0] / 64.0f;
	case 2: return be16(p) / 32768.0f;
	case 3: return (int16_t)be16(p) / 16384.0f;
	default: return asFloat(be32(p));
	}
}

// vertex colors (VAT formats 0-5) into ARGB
uint32_t readColor(const uint8_t* p, int fmt)
{
	switch (fmt)
	{
	case 0: return from565(be16(p));
	case 1: return argb(255, p[0], p[1], p[2]);
	case 2: return argb(255, p[0], p[1], p[2]);
	case 3: { const uint16_t v = be16(p); return argb((v & 15) * 17, (v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17); }
	case 4: { const uint32_t v = (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; return argb((v & 63) * 255 / 63, (v >> 18) * 255 / 63, ((v >> 12) & 63) * 255 / 63, ((v >> 6) & 63) * 255 / 63); }
	default: return argb(p[3], p[0], p[1], p[2]);
	}
}

// an attribute's data: in the vertex (direct) or in its array (indexed: CP 0xA0+n base, 0xB0+n stride)
const uint8_t* indexedData(uint32_t index, int array)
{
	return readRam(s_cp[0xA0 + array] + index * (s_cp[0xB0 + array] & 0xFF), std::max<uint32_t>(s_cp[0xB0 + array] & 0xFF, 36));
}
const uint8_t* attrData(const uint8_t* vtx, int off, int mode, int array)
{
	if (mode == 1)
		return vtx + off;
	const uint32_t index = mode == 2 ? vtx[off] : be16(vtx + off);
	if ((mode == 2 && index == 0xFF) || (mode == 3 && index == 0xFFFF))
		return nullptr;
	return indexedData(index, array);
}

// MATINDEX_A/B: the CP copy (0x30/0x40) or the XF one (0x1018/0x1019; SSA_GX_MATINDEX=cp|xf)
bool matIndexFromXf()
{
	static const int xf = [] {
		const char* e = getenv("SSA_GX_MATINDEX");
		return e && !strcmp(e, "xf") ? 1 : 0;
	}();
	return xf;
}
uint32_t matIndexA()
{
	return matIndexFromXf() ? s_xf[0x1018] : s_cp[0x30];
}
uint32_t matIndexB()
{
	return matIndexFromXf() ? s_xf[0x1019] : s_cp[0x40];
}

// the texture matrix index of texgen n: in the vertex or in MATINDEX_A/B
uint32_t texMatrix(const uint8_t* vtx, const VtxFormat& f, int n)
{
	if (f.texMtxOff[n] >= 0)
		return vtx[f.texMtxOff[n]];
	return n < 4 ? (matIndexA() >> (6 + 6 * n)) & 0x3F : (matIndexB() >> (6 * (n - 4))) & 0x3F;
}

// ---- the transform stage ----
struct Vec3
{
	float x = 0, y = 0, z = 0;
};
Vec3 operator-(Vec3 a, Vec3 b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float dot(Vec3 a, Vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 normalize(Vec3 v)
{
	const float l = std::sqrt(dot(v, v));
	return l > 0 ? Vec3{v.x / l, v.y / l, v.z / l} : v;
}
Vec3 xf3(uint32_t a)
{
	return {asFloat(s_xf[a & 0xFFF]), asFloat(s_xf[(a + 1) & 0xFFF]), asFloat(s_xf[(a + 2) & 0xFFF])};
}

// a GX color register (RGBA, red in the high byte) into 0-255 floats
void rgba(uint32_t v, float out[4])
{
	out[0] = (float)(v >> 24);
	out[1] = (float)((v >> 16) & 0xFF);
	out[2] = (float)((v >> 8) & 0xFF);
	out[3] = (float)(v & 0xFF);
}

// one color channel's color (first = 0, count = 3) or alpha (3, 1) with its control register
// (XF 0x100E+ COLORnCNTRL / 0x1010+ ALPHAnCNTRL: bit 0 material from the vertex, 1 lighting,
// 2-5 and 11-14 the light mask, 6 ambient from the vertex, 7-8 diffuse function, 9-10 attenuation)
void litChannel(uint32_t ctrl, const float vcolor[4], uint32_t matReg, uint32_t ambReg, Vec3 pos, Vec3 nrm, int first, int count, float out[4])
{
	float mat[4], amb[4];
	if (ctrl & 1)
		memcpy(mat, vcolor, sizeof(mat));
	else
		rgba(matReg, mat);
	if (!(ctrl & 2))
	{
		for (int i = first; i < first + count; i++)
			out[i] = mat[i];
		return;
	}
	if (ctrl & 0x40)
		memcpy(amb, vcolor, sizeof(amb));
	else
		rgba(ambReg, amb);
	float lacc[4] = {amb[0], amb[1], amb[2], amb[3]};
	const uint32_t mask = ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
	const uint32_t diffuse = (ctrl >> 7) & 3, attnFn = (ctrl >> 9) & 3;
	for (int l = 0; l < 8; l++)
	{
		if (!(mask & (1u << l)))
			continue;
		const uint32_t base = 0x600 + 16 * l;
		float color[4];
		rgba(s_xf[base + 3], color);
		const Vec3 cosatt = xf3(base + 4), distatt = xf3(base + 7), lpos = xf3(base + 10), ldirv = xf3(base + 13);
		Vec3 ldir;
		float attn = 1.0f;
		if (attnFn == 0 || attnFn == 2)
		{
			ldir = normalize(lpos - pos);
			if (dot(ldir, ldir) == 0)
				ldir = nrm;
		}
		else if (attnFn == 1)
		{
			ldir = normalize(lpos - pos);
			attn = dot(nrm, ldir) >= 0 ? std::max(0.0f, dot(nrm, ldirv)) : 0.0f;
			const Vec3 d = diffuse == 0 ? distatt : normalize(distatt);
			const Vec3 v{1.0f, attn, attn * attn};
			const float den = dot(d, v);
			attn = den != 0 ? std::max(0.0f, dot(cosatt, v)) / den : 0.0f;
		}
		else
		{
			ldir = lpos - pos;
			const float dist2 = dot(ldir, ldir), dist = std::sqrt(dist2);
			if (dist > 0)
				ldir = {ldir.x / dist, ldir.y / dist, ldir.z / dist};
			attn = std::max(0.0f, dot(ldir, ldirv));
			const float den = dot(distatt, Vec3{1.0f, dist, dist2});
			attn = den != 0 ? std::max(0.0f, cosatt.x + cosatt.y * attn + cosatt.z * attn * attn) / den : 0.0f;
		}
		float k = attn;
		if (diffuse == 1)
			k *= dot(ldir, nrm);
		else if (diffuse == 2)
			k *= std::max(0.0f, dot(ldir, nrm));
		for (int i = first; i < first + count; i++)
			lacc[i] += std::round(k * color[i]);
	}
	for (int i = first; i < first + count; i++)
	{
		const float a = std::clamp(lacc[i], 0.0f, 255.0f);
		out[i] = std::floor(mat[i] * (a + std::floor(a / 128.0f)) / 256.0f);
	}
}

uint32_t pack(const float c[4])
{
	auto b = [](float v) { return (uint32_t)std::clamp((int)std::lround(v), 0, 255); };
	return b(c[0]) | b(c[1]) << 8 | b(c[2]) << 16 | b(c[3]) << 24;
}

// one vertex into the backend's layout: clip position, colors 0 and 1, numTex texture coordinates
void transform(const uint8_t* vtx, const VtxFormat& f, int numTex, uint32_t* out)
{
	float pos[3] = {0, 0, 0};
	bool skipped = false; // (a position index of all ones: the vertex is dropped, as in Dolphin)
	if (const uint8_t* p = f.posOff >= 0 ? attrData(vtx, f.posOff, f.posMode, 0) : nullptr)
		for (int i = 0; i < f.posCount; i++)
			pos[i] = readComp(p + i * compSize(f.posFmt), f.posFmt, f.posShift);
	else if (f.posMode >= 2)
	{
		const uint32_t index = f.posMode == 2 ? vtx[f.posOff] : be16(vtx + f.posOff);
		if ((f.posMode == 2 && index == 0xFF) || (f.posMode == 3 && index == 0xFFFF))
		{
			s_frameStats.posSkip++;
			skipped = true;
		}
		else
			s_frameStats.posNull++;
	}
	if (f.pnmtxOff >= 0)
	{
		s_frameStats.pnmtxVerts++;
		if (vtx[f.pnmtxOff] % 3)
			s_frameStats.pnmtxOdd++;
	}
	// normal, binormal, tangent
	Vec3 nbt[3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
	if (f.nrmOff >= 0)
	{
		const int cs = compSize(f.nrmFmt);
		for (int k = 0; k < f.nrmCount; k++)
		{
			const uint8_t* p = nullptr;
			if (f.nrmMode == 1)
				p = vtx + f.nrmOff + k * 3 * cs;
			else if (f.nrmIndex3)
			{
				const int isz = f.nrmMode == 2 ? 1 : 2;
				const uint32_t index = f.nrmMode == 2 ? vtx[f.nrmOff + k * isz] : be16(vtx + f.nrmOff + k * isz);
				if (const uint8_t* q = indexedData(index, 1))
					p = q + k * 3 * cs; // (as Dolphin: element k at its index, k * 3 components on)
			}
			else if (const uint8_t* q = attrData(vtx, f.nrmOff, f.nrmMode, 1))
				p = q + k * 3 * cs;
			if (p)
				nbt[k] = {readNormalComp(p, f.nrmFmt), readNormalComp(p + cs, f.nrmFmt), readNormalComp(p + 2 * cs, f.nrmFmt)};
		}
	}
	// vertex colors (white when missing)
	float vcol[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
	bool hasCol[2] = {false, false};
	for (int c = 0; c < 2; c++)
		if (f.colOff[c] >= 0)
			if (const uint8_t* p = attrData(vtx, f.colOff[c], f.colMode[c], 2 + c))
			{
				const uint32_t v = readColor(p, f.colFmt[c]);
				vcol[c][0] = (float)((v >> 16) & 0xFF);
				vcol[c][1] = (float)((v >> 8) & 0xFF);
				vcol[c][2] = (float)(v & 0xFF);
				vcol[c][3] = (float)(v >> 24);
				hasCol[c] = true;
			}
	// position and normal matrices
	const uint32_t mtx = (f.pnmtxOff >= 0 ? vtx[f.pnmtxOff] : matIndexA()) & 0x3F;
	const uint32_t* m = &s_xf[mtx * 4];
	float v[3];
	for (int r = 0; r < 3; r++)
		v[r] = asFloat(m[r * 4]) * pos[0] + asFloat(m[r * 4 + 1]) * pos[1] + asFloat(m[r * 4 + 2]) * pos[2] + asFloat(m[r * 4 + 3]);
	const uint32_t nm = 0x400 + (mtx & 31) * 3;
	auto nrmXf = [&](Vec3 n) {
		return Vec3{asFloat(s_xf[nm]) * n.x + asFloat(s_xf[nm + 1]) * n.y + asFloat(s_xf[nm + 2]) * n.z,
			asFloat(s_xf[nm + 3]) * n.x + asFloat(s_xf[nm + 4]) * n.y + asFloat(s_xf[nm + 5]) * n.z,
			asFloat(s_xf[nm + 6]) * n.x + asFloat(s_xf[nm + 7]) * n.y + asFloat(s_xf[nm + 8]) * n.z};
	};
	const Vec3 viewPos{v[0], v[1], v[2]};
	const Vec3 viewNrm = normalize(nrmXf(nbt[0]));

	// color channels (XF 0x1009: how many)
	const uint32_t numChans = s_xf[0x1009] & 3;
	float col[2][4];
	for (int c = 0; c < 2; c++)
	{
		if ((uint32_t)c < numChans)
		{
			litChannel(s_xf[0x100E + c], vcol[c], s_xf[0x100C + c], s_xf[0x100A + c], viewPos, viewNrm, 0, 3, col[c]);
			litChannel(s_xf[0x1010 + c], vcol[c], s_xf[0x100C + c], s_xf[0x100A + c], viewPos, viewNrm, 3, 1, col[c]);
		}
		else if (c == 1 && numChans == 1 && !hasCol[1])
			memcpy(col[1], col[0], sizeof(col[1]));
		else
			memcpy(col[c], vcol[c], sizeof(col[c]));
	}

	// projection (XF 0x1020-0x1025, type at 0x1026: 0 perspective, 1 orthographic)
	const float* pr = (const float*)&s_xf[0x1020];
	float cx, cy, cz, cw;
	if (s_xf[0x1026] == 0)
	{
		cx = pr[0] * v[0] + pr[1] * v[2];
		cy = pr[2] * v[1] + pr[3] * v[2];
		cz = pr[4] * v[2] + pr[5];
		cw = -v[2];
	}
	else
	{
		cx = pr[0] * v[0] + pr[1];
		cy = pr[2] * v[1] + pr[3];
		cz = pr[4] * v[2] + pr[5];
		cw = 1.0f;
	}
	// to Vulkan clip space over the 640x528 EFB: the viewport (XF 0x101A-0x101F: width, height, z
	// range, x origin, y origin, far z) less the scissor offset (BP 0x59), depth in 2^24 units
	const float* vp = (const float*)&s_xf[0x101A];
	const float xoff = (float)((s_bp[0x59] & 0x3FF) * 2), yoff = (float)(((s_bp[0x59] >> 10) & 0x3FF) * 2);
	const float clip[4] = {(cx * vp[0] + (vp[3] - xoff) * cw) / 320.0f - cw, (cy * vp[1] + (vp[4] - yoff) * cw) / 264.0f - cw,
		(cz * vp[2] + vp[5] * cw) / 16777216.0f, cw};
	for (int i = 0; i < 4; i++)
		out[i] = skipped ? 0x7FC00000u : asBits(clip[i]);
	out[4] = pack(col[0]);
	out[5] = pack(col[1]);

	// texgens (XF 0x1040+n: bit 1 STQ, bit 2 input form ABC1, bits 4-6 type (regular, emboss,
	// color 0, color 1), bits 7-11 source row, 12-14 emboss source); post-transform (XF 0x1050+n)
	// with dual texture transforms on (XF 0x1012 bit 0)
	float tc[8][3];
	const bool dual = s_xf[0x1012] & 1;
	for (int n = 0; n < numTex; n++)
	{
		const uint32_t info = s_xf[0x1040 + n];
		const uint32_t type = (info >> 4) & 7, row = (info >> 7) & 31;
		float* o = tc[n];
		if (type == 2 || type == 3)
		{
			const float* c = col[type - 2];
			o[0] = c[0] / 255.0f;
			o[1] = c[1] / 255.0f;
			o[2] = 1.0f;
			continue;
		}
		if (type == 1)
		{
			const uint32_t src = (info >> 12) & 7;
			if (src < (uint32_t)n)
				memcpy(o, tc[src], sizeof(tc[src]));
			else
				o[0] = o[1] = 0, o[2] = 1;
			continue;
		}
		float in[4] = {0, 0, 1, 1};
		if (row == 0)
			in[0] = pos[0], in[1] = pos[1], in[2] = pos[2];
		else if (row == 1)
			in[0] = nbt[0].x, in[1] = nbt[0].y, in[2] = nbt[0].z;
		else if (row == 2)
			in[0] = col[0][0] / 255.0f, in[1] = col[0][1] / 255.0f, in[2] = col[0][2] / 255.0f;
		else if (row == 3 || row == 4)
		{
			const Vec3& b = nbt[row == 3 ? 2 : 1];
			in[0] = b.x, in[1] = b.y, in[2] = b.z;
		}
		else if (row >= 5 && row < 13)
		{
			const int k = row - 5;
			if (f.texOff[k] >= 0)
				if (const uint8_t* p = attrData(vtx, f.texOff[k], f.texMode[k], 4 + k))
					for (int i = 0; i < f.texCount[k]; i++)
						in[i] = readComp(p + i * compSize(f.texFmt[k]), f.texFmt[k], f.texShift[k]);
		}
		if (!((info >> 2) & 1))
			in[2] = 1.0f; // (AB11)
		const uint32_t* t = &s_xf[texMatrix(vtx, f, n) * 4];
		auto rowDot = [&](const uint32_t* r) { return asFloat(r[0]) * in[0] + asFloat(r[1]) * in[1] + asFloat(r[2]) * in[2] + asFloat(r[3]) * in[3]; };
		o[0] = rowDot(t);
		o[1] = rowDot(t + 4);
		o[2] = (info & 2) ? rowDot(t + 8) : 1.0f;
		if (dual)
		{
			const uint32_t post = s_xf[0x1050 + n];
			Vec3 s{o[0], o[1], o[2]};
			if ((post >> 8) & 1)
				s = normalize(s);
			const uint32_t* pm = &s_xf[0x500 + (post & 0x3F) * 4];
			auto postDot = [&](const uint32_t* r) { return asFloat(r[0]) * s.x + asFloat(r[1]) * s.y + asFloat(r[2]) * s.z + asFloat(r[3]); };
			o[0] = postDot(pm);
			o[1] = postDot(pm + 4);
			o[2] = postDot(pm + 8);
		}
	}
	for (int n = 0; n < numTex; n++)
		for (int i = 0; i < 3; i++)
			out[6 + 3 * n + i] = asBits(tc[n][i]);
}

// ---- the GPU transform (tev.vert): vertices fetched and decoded, XF snapshots ----
// the vertex layout of a format (tev.vert's word 0 without the XF index) and its size in words
uint32_t fetchLayout(const VtxFormat& f, uint32_t* words)
{
	bool perVertexMtx = f.pnmtxOff >= 0;
	for (int t = 0; t < 8; t++)
		perVertexMtx |= f.texMtxOff[t] >= 0;
	const uint32_t nrm = f.nrmOff >= 0 ? (uint32_t)f.nrmCount : 0;
	uint32_t texMask = 0;
	for (int t = 0; t < 8; t++)
		if (f.texOff[t] >= 0)
			texMask |= 1u << t;
	*words = 1 + 3 + (perVertexMtx ? 2 : 0) + 3 * nrm + 2 + 2 * (uint32_t)__builtin_popcount(texMask);
	return (perVertexMtx ? 1u << 16 : 0) | nrm << 17 | (f.colOff[1] >= 0 ? 1u << 19 : 0) | texMask << 20;
}

// one vertex, decoded (tev.vert's layout); meta: the layout word with the XF index
void fetch(const uint8_t* vtx, const VtxFormat& f, uint32_t meta, uint32_t* out)
{
	uint32_t* o = out + 4;
	float pos[3] = {0, 0, 0};
	if (const uint8_t* p = f.posOff >= 0 ? attrData(vtx, f.posOff, f.posMode, 0) : nullptr)
		for (int i = 0; i < f.posCount; i++)
			pos[i] = readComp(p + i * compSize(f.posFmt), f.posFmt, f.posShift);
	else if (f.posMode >= 2)
	{
		const uint32_t index = f.posMode == 2 ? vtx[f.posOff] : be16(vtx + f.posOff);
		if ((f.posMode == 2 && index == 0xFF) || (f.posMode == 3 && index == 0xFFFF))
		{
			s_frameStats.posSkip++;
			meta |= 1u << 30; // (dropped, as in Dolphin)
		}
		else
			s_frameStats.posNull++;
	}
	out[0] = meta;
	for (int i = 0; i < 3; i++)
		out[1 + i] = asBits(pos[i]);
	if (meta & (1u << 16))
	{
		uint32_t pn = (f.pnmtxOff >= 0 ? vtx[f.pnmtxOff] : matIndexA()) & 0x3F;
		uint32_t w1 = 0;
		for (int n = 0; n < 8; n++)
		{
			const uint32_t t = texMatrix(vtx, f, n) & 0x3F;
			if (n < 4)
				pn |= t << (6 + 6 * n);
			else
				w1 |= t << (6 * (n - 4));
		}
		*o++ = pn;
		*o++ = w1;
	}
	if (f.nrmOff >= 0)
	{
		static const float defaults[3][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
		const int cs = compSize(f.nrmFmt);
		for (int k = 0; k < f.nrmCount; k++)
		{
			const uint8_t* p = nullptr;
			if (f.nrmMode == 1)
				p = vtx + f.nrmOff + k * 3 * cs;
			else if (f.nrmIndex3)
			{
				const int isz = f.nrmMode == 2 ? 1 : 2;
				const uint32_t index = f.nrmMode == 2 ? vtx[f.nrmOff + k * isz] : be16(vtx + f.nrmOff + k * isz);
				if (const uint8_t* q = indexedData(index, 1))
					p = q + k * 3 * cs;
			}
			else if (const uint8_t* q = attrData(vtx, f.nrmOff, f.nrmMode, 1))
				p = q + k * 3 * cs;
			for (int i = 0; i < 3; i++)
				*o++ = asBits(p ? readNormalComp(p + i * cs, f.nrmFmt) : defaults[k][i]);
		}
	}
	for (int c = 0; c < 2; c++)
	{
		uint32_t v = 0xFFFFFFFFu;
		if (f.colOff[c] >= 0)
			if (const uint8_t* p = attrData(vtx, f.colOff[c], f.colMode[c], 2 + c))
			{
				const uint32_t a = readColor(p, f.colFmt[c]); // (ARGB)
				v = ((a >> 16) & 0xFF) | (a & 0xFF00) | (a & 0xFF) << 16 | (a & 0xFF000000u);
			}
		*o++ = v;
	}
	for (int k = 0; k < 8; k++)
		if (f.texOff[k] >= 0)
		{
			float st[2] = {0, 0};
			if (const uint8_t* p = attrData(vtx, f.texOff[k], f.texMode[k], 4 + k))
				for (int i = 0; i < f.texCount[k]; i++)
					st[i] = readComp(p + i * compSize(f.texFmt[k]), f.texFmt[k], f.texShift[k]);
			*o++ = asBits(st[0]);
			*o++ = asBits(st[1]);
		}
}

// ---- the fast fetch: a draw's vertices with its arrays resolved once and decoders per format
// (fetch() above is the reference: used while recording, since that reads RAM through readRam, and
// with SSA_GX_SLOWFETCH=1) ----
template <int F>
inline float comp(const uint8_t* p, float scale)
{
	if constexpr (F == 0)
		return p[0] * scale;
	else if constexpr (F == 1)
		return (int8_t)p[0] * scale;
	else if constexpr (F == 2)
		return be16(p) * scale;
	else if constexpr (F == 3)
		return (int16_t)be16(p) * scale;
	else
		return asFloat(be32(p));
}
typedef void (*CompsFn)(const uint8_t* p, int n, float scale, uint32_t* out);
template <int F>
void comps(const uint8_t* p, int n, float scale, uint32_t* out)
{
	constexpr int cs = F >= 4 ? 4 : F >= 2 ? 2 : 1;
	for (int i = 0; i < n; i++)
		out[i] = asBits(comp<F>(p + i * cs, scale));
}
CompsFn compsFn(int fmt)
{
	static const CompsFn fns[5] = {comps<0>, comps<1>, comps<2>, comps<3>, comps<4>};
	return fns[std::min(fmt, 4)];
}

// an attribute's source: in the vertex (mode 1) or an array resolved for the draw
struct Src
{
	int off = -1, mode = 0;
	const uint8_t* base = nullptr;
	uint64_t stride = 0, avail = 0, bytes = 0;
};
Src source(int off, int mode, int array, uint32_t bytes)
{
	Src r;
	r.off = off;
	r.mode = mode;
	if (mode >= 2)
	{
		uint32_t av = 0;
		r.base = phys(s_cp[0xA0 + array], &av);
		r.stride = s_cp[0xB0 + array] & 0xFF;
		r.avail = av;
		r.bytes = bytes;
	}
	return r;
}
inline const uint8_t* at(const Src& s, const uint8_t* vtx, int indexOff = 0)
{
	if (s.mode == 1)
		return vtx + s.off;
	const uint32_t i = s.mode == 2 ? vtx[s.off + indexOff] : be16(vtx + s.off + indexOff);
	if (s.mode == 2 ? i == 0xFF : i == 0xFFFF)
		return nullptr;
	const uint64_t o = i * s.stride;
	return s.base && o + s.bytes <= s.avail ? s.base + o : nullptr;
}

// what a draw's vertex fetch needs besides its vertex bytes: the format, its attributes' sources
// (arrays resolved from CP 0xA0+/0xB0+), decoders and scales, the default matrix indices. Made on the
// GX thread (from its registers); run on it or on a fetch worker.
struct FetchPlan
{
	VtxFormat f;
	Src pos, nrm, col[2], tex[8];
	CompsFn posFn = nullptr, nrmFn = nullptr, texFn[8] = {};
	float posScale = 1, nrmScale = 1, texScale[8] = {};
	uint32_t texMask = 0, defMtx[9] = {};
	int ncs = 1;
};

void makePlan(FetchPlan& p, const VtxFormat& f)
{
	p.f = f;
	p.pos = source(f.posOff, f.posMode, 0, f.posCount * compSize(f.posFmt));
	p.posFn = compsFn(f.posFmt);
	p.posScale = 1.0f / (float)(1u << f.posShift);
	p.ncs = compSize(f.nrmFmt);
	p.nrm = f.nrmOff >= 0 ? source(f.nrmOff, f.nrmMode, 1, (f.nrmIndex3 ? 3 : f.nrmCount) * 3 * p.ncs) : Src{};
	p.nrmFn = compsFn(f.nrmFmt);
	static const float kNrmScale[5] = {1 / 128.0f, 1 / 64.0f, 1 / 32768.0f, 1 / 16384.0f, 1.0f};
	p.nrmScale = kNrmScale[std::min(f.nrmFmt, 4)];
	for (int c = 0; c < 2; c++)
		p.col[c] = f.colOff[c] >= 0 ? source(f.colOff[c], f.colMode[c], 2 + c, (uint32_t)kColorSize[f.colFmt[c]]) : Src{};
	p.texMask = 0;
	for (int k = 0; k < 8; k++)
		if (f.texOff[k] >= 0)
		{
			p.texMask |= 1u << k;
			p.tex[k] = source(f.texOff[k], f.texMode[k], 4 + k, f.texCount[k] * compSize(f.texFmt[k]));
			p.texFn[k] = compsFn(f.texFmt[k]);
			p.texScale[k] = 1.0f / (float)(1u << f.texShift[k]);
		}
		else
			p.tex[k] = Src{};
	// matrix indices: MATINDEX_A/B, each replaced by the vertex's own where it has one
	p.defMtx[0] = matIndexA() & 0x3F;
	for (int n = 0; n < 8; n++)
		p.defMtx[1 + n] = n < 4 ? (matIndexA() >> (6 + 6 * n)) & 0x3F : (matIndexB() >> (6 * (n - 4))) & 0x3F;
}

// the counts a fetch adds to the frame's statistics
struct FetchCounts
{
	uint64_t posSkip = 0, posNull = 0;
};

void runFetch(const FetchPlan& P, const uint8_t* data, int count, uint32_t meta, uint32_t stride, uint32_t* out, FetchCounts& counts)
{
	const VtxFormat& f = P.f;
	const Src& pos = P.pos;
	const CompsFn posFn = P.posFn;
	const float posScale = P.posScale;
	const int ncs = P.ncs;
	const Src& nrm = P.nrm;
	const CompsFn nrmFn = P.nrmFn;
	const float nrmScale = P.nrmScale;
	const Src* col = P.col;
	const Src* tex = P.tex;
	const CompsFn* texFn = P.texFn;
	const float* texScale = P.texScale;
	const uint32_t texMask = P.texMask;
	const uint32_t* defMtx = P.defMtx;
	const bool mtxWords = meta & (1u << 16);
	static const float kDefNbt[3][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};

	for (int v = 0; v < count; v++, data += f.size, out += stride)
	{
		const uint8_t* vtx = data;
		uint32_t m = meta;
		if (const uint8_t* p = f.posOff >= 0 ? at(pos, vtx) : nullptr)
		{
			if (f.posCount == 2)
				out[3] = 0;
			posFn(p, f.posCount, posScale, out + 1);
		}
		else
		{
			out[1] = out[2] = out[3] = 0;
			if (f.posMode >= 2)
			{
				const uint32_t index = f.posMode == 2 ? vtx[f.posOff] : be16(vtx + f.posOff);
				if (f.posMode == 2 ? index == 0xFF : index == 0xFFFF)
				{
					counts.posSkip++;
					m |= 1u << 30;
				}
				else
					counts.posNull++;
			}
		}
		out[0] = m;
		uint32_t* o = out + 4;
		if (mtxWords)
		{
			uint32_t i[9];
			i[0] = f.pnmtxOff >= 0 ? vtx[f.pnmtxOff] & 0x3F : defMtx[0];
			for (int n = 0; n < 8; n++)
				i[1 + n] = f.texMtxOff[n] >= 0 ? vtx[f.texMtxOff[n]] & 0x3F : defMtx[1 + n];
			o[0] = i[0] | i[1] << 6 | i[2] << 12 | i[3] << 18 | i[4] << 24;
			o[1] = i[5] | i[6] << 6 | i[7] << 12 | i[8] << 18;
			o += 2;
		}
		if (f.nrmOff >= 0)
		{
			const int isz = f.nrmMode == 2 ? 1 : 2;
			for (int k = 0; k < f.nrmCount; k++)
			{
				const uint8_t* p = f.nrmIndex3 && f.nrmMode >= 2 ? at(nrm, vtx, k * isz) : at(nrm, vtx);
				if (p)
					nrmFn(p + k * 3 * ncs, 3, nrmScale, o);
				else
					for (int i = 0; i < 3; i++)
						o[i] = asBits(kDefNbt[k][i]);
				o += 3;
			}
		}
		for (int c = 0; c < 2; c++)
		{
			uint32_t w = 0xFFFFFFFFu;
			if (f.colOff[c] >= 0)
				if (const uint8_t* p = at(col[c], vtx))
				{
					const uint32_t a = readColor(p, f.colFmt[c]);
					w = ((a >> 16) & 0xFF) | (a & 0xFF00) | (a & 0xFF) << 16 | (a & 0xFF000000u);
				}
			*o++ = w;
		}
		for (uint32_t k = 0, mask = texMask; mask; k++, mask >>= 1)
			if (mask & 1)
			{
				if (const uint8_t* p = at(tex[k], vtx))
				{
					if (f.texCount[k] == 1)
						o[1] = 0;
					texFn[k](p, f.texCount[k], texScale[k], o);
				}
				else
					o[0] = o[1] = 0;
				o += 2;
			}
	}
}

// ---- vertex fetch workers (SSA_GX_WORKERS=n; the PS4's default 3) ----
// The GX thread reserves each draw's vertices in the ring at once (their size is known: count x stride
// words) and writes its indices, which name word offsets only; the fetch itself (reading the arrays in
// RAM, decoding to floats) becomes a job: the plan, the draw's vertex bytes copied out of the FIFO,
// the place in the ring. Workers take the jobs in order; gxFetchFinish (before every submit, and when
// the job list is full) runs what is left on the GX thread itself and waits for the rest.
struct FetchJob
{
	const FetchPlan* plan;
	const uint8_t* data;
	uint32_t* out;
	int count;
	uint32_t meta, stride;
};
constexpr uint32_t kMaxJobs = 1u << 16;
FetchJob s_jobs[kMaxJobs];
std::atomic<uint64_t> s_jobClaim{0}, s_jobPub{0}; // (epoch << 32) | job index: the next to take / the published count
std::atomic<uint32_t> s_jobDone{0};
std::atomic<uint64_t> s_jobSkip{0}, s_jobNull{0};
uint32_t s_jobEpoch = 0, s_jobCount = 0;
int s_workers = -1;
// the plans and vertex bytes of jobs not yet finished (kept until gxFetchFinish)
std::deque<FetchPlan> s_plans;
constexpr size_t kChunk = 1u << 20;
std::vector<uint8_t*> s_chunks;
size_t s_chunkIndex = 0, s_chunkUsed = 0;
// the plan the last draw used and what it was made from (CP VCD/VAT, arrays, MATINDEX)
struct PlanKey
{
	uint32_t w[32];
	bool operator==(const PlanKey& o) const { return !memcmp(w, o.w, sizeof(w)); }
};
PlanKey s_planKey;
const FetchPlan* s_plan = nullptr;
// (the GX thread's time in ticks: fetch or job making, waiting for workers, recording draws, EFB
// copies to textures, frame ends)
uint64_t s_ticksFetch = 0, s_ticksFinish = 0, s_ticksDraw = 0, s_ticksCopy = 0, s_ticksFrame = 0;

void runJob(const FetchJob& j)
{
	FetchCounts c;
	runFetch(*j.plan, j.data, j.count, j.meta, j.stride, j.out, c);
	if (c.posSkip)
		s_jobSkip.fetch_add(c.posSkip, std::memory_order_relaxed);
	if (c.posNull)
		s_jobNull.fetch_add(c.posNull, std::memory_order_relaxed);
}

// one job taken (false: none published)
bool takeJob()
{
	uint64_t c = s_jobClaim.load(std::memory_order_acquire);
	const uint64_t p = s_jobPub.load(std::memory_order_acquire);
	if ((c >> 32) != (p >> 32) || (uint32_t)c >= (uint32_t)p)
		return false;
	if (!s_jobClaim.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel))
		return true; // (another took it: look again)
	runJob(s_jobs[(uint32_t)c]);
	s_jobDone.fetch_add(1, std::memory_order_release);
	return true;
}

void fetchWorker(int core)
{
	if (g_ssaPinThread)
		g_ssaPinThread(core, 660, "gx-fetch");
	int idle = 0;
	for (;;)
	{
		if (takeJob())
		{
			idle = 0;
			continue;
		}
		if (++idle < 400)
			__builtin_ia32_pause();
		else if (idle < 600)
			std::this_thread::yield();
		else
			std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
}

uint8_t* jobBytes(size_t n)
{
	if (n > kChunk)
		return nullptr;
	if (s_chunkIndex >= s_chunks.size() || s_chunkUsed + n > kChunk)
	{
		if (s_chunkIndex < s_chunks.size() && s_chunkUsed)
			s_chunkIndex++;
		if (s_chunkIndex >= s_chunks.size())
			s_chunks.push_back((uint8_t*)malloc(kChunk));
		s_chunkUsed = 0;
	}
	uint8_t* p = s_chunks[s_chunkIndex] + s_chunkUsed;
	s_chunkUsed += (n + 15) & ~(size_t)15;
	return p;
}

int workerCount()
{
	if (s_workers < 0)
	{
		const char* e = getenv("SSA_GX_WORKERS");
		s_workers = e ? std::clamp(atoi(e), 0, 6) : 0;
		if (s_rec || getenv("SSA_GX_SLOWFETCH") || getenv("SSA_GX_CPUXF"))
			s_workers = 0;
		for (int i = 0; i < s_workers; i++)
			std::thread(fetchWorker, 2 + i).detach();
		if (s_workers)
			fprintf(stderr, "gx: %d vertex fetch workers\n", s_workers);
	}
	return s_workers;
}

// the plan for a draw with this VAT: the last one while its registers are the same
const FetchPlan& planFor(int vat)
{
	PlanKey k;
	k.w[0] = s_cp[0x50];
	k.w[1] = s_cp[0x60];
	k.w[2] = s_cp[0x70 + vat];
	k.w[3] = s_cp[0x80 + vat];
	k.w[4] = s_cp[0x90 + vat];
	for (int i = 0; i < 12; i++)
	{
		k.w[5 + i] = s_cp[0xA0 + i];
		k.w[17 + i] = s_cp[0xB0 + i];
	}
	k.w[29] = matIndexA();
	k.w[30] = matIndexB();
	k.w[31] = (uint32_t)vat;
	if (s_plan && k == s_planKey)
		return *s_plan;
	s_plans.emplace_back();
	makePlan(s_plans.back(), cachedFormat(vat));
	s_plan = &s_plans.back();
	s_planKey = k;
	return *s_plan;
}
} // namespace

// every vertex reserved so far filled (the GX thread, before a submit)
void gxFetchFinish()
{
	const uint64_t t0 = __builtin_ia32_rdtsc();
	if (s_jobCount)
	{
		while (takeJob())
		{
		}
		int spins = 0;
		while (s_jobDone.load(std::memory_order_acquire) < s_jobCount)
			if (++spins < 1000)
				__builtin_ia32_pause();
			else
				std::this_thread::yield();
		// a new epoch: the old claims can't take the new jobs
		s_jobEpoch++;
		s_jobCount = 0;
		s_jobDone.store(0, std::memory_order_relaxed);
		s_jobClaim.store((uint64_t)s_jobEpoch << 32, std::memory_order_release);
		s_jobPub.store((uint64_t)s_jobEpoch << 32, std::memory_order_release);
		s_frameStats.posSkip += s_jobSkip.exchange(0);
		s_frameStats.posNull += s_jobNull.exchange(0);
	}
	s_plans.clear();
	s_plan = nullptr;
	s_chunkIndex = 0;
	s_chunkUsed = 0;
	s_ticksFinish += __builtin_ia32_rdtsc() - t0;
}

namespace
{
// a draw's fetch: at once, or as a job for the workers (its vertex bytes copied)
void fetchOrQueue(const FetchPlan& plan, const uint8_t* data, int count, uint32_t meta, uint32_t stride, uint32_t* out)
{
	// (the job list full, or a draw too big for a chunk: fetched here; finishing the jobs now would let
	// this draw's plan go)
	if (workerCount() > 0 && s_jobCount < kMaxJobs)
	{
		const size_t bytes = (size_t)count * plan.f.size;
		if (uint8_t* copy = jobBytes(bytes))
		{
			memcpy(copy, data, bytes);
			s_jobs[s_jobCount] = {&plan, copy, out, count, meta, stride};
			s_jobCount++;
			s_jobPub.store((uint64_t)s_jobEpoch << 32 | s_jobCount, std::memory_order_release);
			return;
		}
	}
	FetchCounts c;
	runFetch(plan, data, count, meta, stride, out, c);
	s_frameStats.posSkip += c.posSkip;
	s_frameStats.posNull += c.posNull;
}

// the XF snapshot (tev.vert's layout)
void fillXf(uint32_t* x)
{
	memcpy(x, &s_xf[0x000], 256 * 4);
	memcpy(x + 256, &s_xf[0x400], 128 * 4);
	memcpy(x + 384, &s_xf[0x500], 256 * 4);
	memcpy(x + 640, &s_xf[0x600], 128 * 4);
	memcpy(x + 768, &s_xf[0x1000], 96 * 4);
	x[768 + 0x18] = matIndexA();
	x[768 + 0x19] = matIndexB();
	x[864] = asBits((float)((s_bp[0x59] & 0x3FF) * 2));
	x[865] = asBits((float)(((s_bp[0x59] >> 10) & 0x3FF) * 2));
	x[866] = x[867] = 0;
}

// triangles of a primitive (0 quads, 1 quads 2, 2 triangles, 3 strip, 4 fan); lines and points: 0
uint32_t triangleCount(int primitive, int count)
{
	switch (primitive)
	{
	case 0: case 1: return (uint32_t)(count / 4) * 2;
	case 2: return (uint32_t)(count / 3);
	case 3: case 4: return count >= 3 ? (uint32_t)(count - 2) : 0;
	default: return 0;
	}
}

void draw(int primitive, int vat, const uint8_t* data, int count)
{
	s_frameStats.draws++;
	s_frameStats.vertices += count;
	if (primitive >= 5)
		s_stats.lines++;
	if (!gxvk::rendering())
		return;
	// (SSA_GX_SKIP=lo-hi: leave out a frame's draws lo..hi, for finding a draw in gx_replay;
	// SSA_GX_INFO=lo-hi: log those draws' formats)
	static uint64_t skipLo = ~0ull, skipHi = 0, infoLo = ~0ull, infoHi = 0;
	static bool parsed = false;
	if (!parsed)
	{
		parsed = true;
		if (const char* e = getenv("SSA_GX_SKIP"))
			sscanf(e, "%llu-%llu", (unsigned long long*)&skipLo, (unsigned long long*)&skipHi);
		if (const char* e = getenv("SSA_GX_INFO"))
			sscanf(e, "%llu-%llu", (unsigned long long*)&infoLo, (unsigned long long*)&infoHi);
	}
	static uint64_t onlyLo = 0, onlyHi = ~0ull;
	static bool onlyParsed = false;
	if (!onlyParsed)
	{
		onlyParsed = true;
		if (const char* e = getenv("SSA_GX_ONLY"))
			sscanf(e, "%llu-%llu", (unsigned long long*)&onlyLo, (unsigned long long*)&onlyHi);
	}
	const uint64_t n = s_frameStats.draws;
	if ((n >= skipLo && n <= skipHi) || n < onlyLo || n > onlyHi)
		return;
	if (n >= infoLo && n <= infoHi)
	{
		const VtxFormat v = vertexFormat(vat);
		fprintf(stderr, "gx: draw %llu: prim %d vat %d count %d size %d pos off %d mode %d fmt %d n %d shift %d nrm %d/%d col0 %d/%d tex0 %d/%d fmt %d shift %d pnmtx %d CP30 %08x VCD %08x %08x VAT %08x %08x %08x\n",
			(unsigned long long)n, primitive, vat, count, v.size, v.posOff, v.posMode, v.posFmt, v.posCount, v.posShift, v.nrmOff, v.nrmMode, v.colOff[0],
			v.colMode[0], v.texOff[0], v.texMode[0], v.texFmt[0], v.texShift[0], v.pnmtxOff, s_cp[0x30], s_cp[0x50], s_cp[0x60], s_cp[0x70 + vat],
			s_cp[0x80 + vat], s_cp[0x90 + vat]);
		fprintf(stderr, "    pos array %08x stride %u, nrm array %08x stride %u, tex0 array %08x stride %u; vertices:", s_cp[0xA0], s_cp[0xB0] & 0xFF, s_cp[0xA1],
			s_cp[0xB1] & 0xFF, s_cp[0xA4], s_cp[0xB4] & 0xFF);
		for (int i = 0; i < std::min(count, 6); i++)
		{
			const uint8_t* vtx = data + (size_t)i * v.size;
			const uint32_t index = v.posMode == 2 ? vtx[v.posOff] : v.posMode == 3 ? be16(vtx + v.posOff) : 0;
			float pos[3] = {0, 0, 0};
			if (const uint8_t* p = attrData(vtx, v.posOff, v.posMode, 0))
				for (int k = 0; k < v.posCount; k++)
					pos[k] = readComp(p + k * compSize(v.posFmt), v.posFmt, v.posShift);
			fprintf(stderr, " [%u] %.2f %.2f %.2f", index, pos[0], pos[1], pos[2]);
		}
		fprintf(stderr, "\n");
		// the textures maps 0-3 hold (SETIMAGE0: width-1, height-1, format; SETIMAGE3: address / 32;
		// SETMODE0: wrap s/t, filters), and the first vertices' texture coordinates
		fprintf(stderr, "    textures:");
		for (int t = 0; t < 4; t++)
		{
			const uint32_t i0 = s_bp[0x88 + t], i3 = s_bp[0x94 + t], m0 = s_bp[0x80 + t];
			fprintf(stderr, " [%d] %ux%u fmt %u at %08x wrap %u/%u img1 %06x", t, (i0 & 0x3FF) + 1, ((i0 >> 10) & 0x3FF) + 1, (i0 >> 20) & 15, (i3 & 0xFFFFFF) << 5,
				m0 & 3, (m0 >> 2) & 3, s_bp[0x8C + t] & 0xFFFFFF);
		}
		// the TEV stages' orders (map, coordinate, enabled), the texgens (XF 0x1040+n, post 0x1050+n)
		// and the texture matrix indices (CP 0x30/0x40)
		{
			const uint32_t stages = ((s_bp[0x00] >> 10) & 15) + 1, gens = s_xf[0x103F] & 15;
			fprintf(stderr, "\n    %u stages:", stages);
			for (uint32_t n = 0; n < stages; n++)
			{
				const uint32_t tref = s_bp[0x28 + (n >> 1)] >> ((n & 1) * 12);
				fprintf(stderr, " %u:map%u/coord%u%s", n, tref & 7, (tref >> 3) & 7, (tref & 0x40) ? "" : "(off)");
			}
			fprintf(stderr, "; %u texgens:", gens);
			for (uint32_t n = 0; n < gens; n++)
				fprintf(stderr, " %u:%08x/%08x", n, s_xf[0x1040 + n], s_xf[0x1050 + n]);
			fprintf(stderr, "; texmtx idx CP30 %08x CP40 %08x\n    PE: GENMODE %06x ZMODE %06x CMODE0 %06x CMODE1 %06x ZCTRL %06x; TEV color/alpha:", s_cp[0x30], s_cp[0x40], s_bp[0x00] & 0xFFFFFF,
				s_bp[0x40] & 0xFFFFFF, s_bp[0x41] & 0xFFFFFF, s_bp[0x42] & 0xFFFFFF, s_bp[0x43] & 0xFFFFFF);
			for (uint32_t n = 0; n < stages; n++)
				fprintf(stderr, " %06x/%06x", s_bp[0xC0 + 2 * n] & 0xFFFFFF, s_bp[0xC1 + 2 * n] & 0xFFFFFF);
			fprintf(stderr, "; alpha compare %06x\n   ", s_bp[0xF3] & 0xFFFFFF);
			// depth: XF 0x1005 (clip disable bits), the projection (0x1020-0x1026), the viewport's
			// z scale / far (0x101C/0x101F), and the first vertices' clip z/w and window depth
			auto xff = [](uint32_t r) { float f; memcpy(&f, &s_xf[r], 4); return f; };
			fprintf(stderr, " XF 1005 %08x proj %s %g %g %g %g %g %g, viewport z %g far %g; clip z/w, depth:", s_xf[0x1005], s_xf[0x1026] ? "ortho" : "persp",
				xff(0x1020), xff(0x1021), xff(0x1022), xff(0x1023), xff(0x1024), xff(0x1025), xff(0x101C), xff(0x101F));
			const uint32_t mtx = matIndexA() & 63;
			for (int i = 0; i < std::min(count, 3); i++)
			{
				const uint8_t* vtx = data + (size_t)i * v.size;
				float pos[3] = {0, 0, 0};
				if (const uint8_t* p = attrData(vtx, v.posOff, v.posMode, 0))
					for (int k = 0; k < v.posCount; k++)
						pos[k] = readComp(p + k * compSize(v.posFmt), v.posFmt, v.posShift);
				const uint32_t m = v.pnmtxOff >= 0 ? (vtx[v.pnmtxOff] & 63) : mtx;
				float e[3];
				for (int r = 0; r < 3; r++)
					e[r] = xff(m * 4 + r * 4) * pos[0] + xff(m * 4 + r * 4 + 1) * pos[1] + xff(m * 4 + r * 4 + 2) * pos[2] + xff(m * 4 + r * 4 + 3);
				const bool ortho = s_xf[0x1026] != 0;
				const float cz = ortho ? xff(0x1024) * e[2] + xff(0x1025) : xff(0x1024) * e[2] + xff(0x1025), cw = ortho ? 1.0f : -e[2];
				fprintf(stderr, " eye z %.2f: %.7f, %.1f", e[2], cz / cw, (cz * xff(0x101C) + xff(0x101F) * cw) / cw);
			}
			fprintf(stderr, "\n   ");
			// the texgens' matrices: XF 0x1012 (dual transform), each one's texture matrix rows and,
			// with dual transform, its post-transform rows
			fprintf(stderr, " dual %x;", s_xf[0x1012]);
			for (uint32_t n = 0; n < gens && n < 4; n++)
			{
				const uint32_t tmi = n < 4 ? (matIndexA() >> (6 + 6 * n)) & 63 : (matIndexB() >> (6 * (n - 4))) & 63;
				fprintf(stderr, " gen%u mtx %u [%g %g %g %g / %g %g %g %g]", n, tmi, xff(tmi * 4), xff(tmi * 4 + 1), xff(tmi * 4 + 2), xff(tmi * 4 + 3),
					xff(tmi * 4 + 4), xff(tmi * 4 + 5), xff(tmi * 4 + 6), xff(tmi * 4 + 7));
				const uint32_t pi = (s_xf[0x1050 + n] & 63) * 4;
				fprintf(stderr, " post %u [%g %g %g %g / %g %g %g %g]", pi / 4, xff(0x500 + pi), xff(0x500 + pi + 1), xff(0x500 + pi + 2), xff(0x500 + pi + 3),
					xff(0x500 + pi + 4), xff(0x500 + pi + 5), xff(0x500 + pi + 6), xff(0x500 + pi + 7));
			}
			fprintf(stderr, "\n   ");
		}
		fprintf(stderr, "; SU sizes (s t) per coordinate:");
		for (int c = 0; c < 8; c++)
			fprintf(stderr, " %d:%08x/%08x", c, s_bp[0x30 + 2 * c], s_bp[0x31 + 2 * c]);
		for (int k = 0; k < 2; k++)
		{
			fprintf(stderr, "\n    tex%d (fmt %d shift %d count %d) uv:", k, v.texFmt[k], v.texShift[k], v.texCount[k]);
			for (int i = 0; i < std::min(count, 4); i++)
			{
				const uint8_t* vtx = data + (size_t)i * v.size;
				if (v.texOff[k] >= 0)
					if (const uint8_t* p = attrData(vtx, v.texOff[k], v.texMode[k], 4 + k))
						fprintf(stderr, " (%.3f %.3f)", readComp(p, v.texFmt[k], v.texShift[k]), readComp(p + compSize(v.texFmt[k]), v.texFmt[k], v.texShift[k]));
			}
		}
		fprintf(stderr, "\n");
	}
	const uint64_t fetchT0 = __builtin_ia32_rdtsc();
	const VtxFormat* fp = &planFor(vat).f;
	const uint32_t tris = triangleCount(primitive, count);
	if (fp->posOff < 0 || !tris)
		return;
	const int numTex = (int)std::min<uint32_t>(s_xf[0x103F] & 15, 8);
	// (SSA_GX_CPUXF=1: transform and light on the CPU, as before tev.vert did it)
	static const bool cpuXf = getenv("SSA_GX_CPUXF") && atoi(getenv("SSA_GX_CPUXF"));
	uint32_t stride = 0;
	const uint32_t layout = cpuXf ? 0x80000000u : fetchLayout(*fp, &stride);
	if (cpuXf)
		stride = 7 + 3 * (uint32_t)numTex;
	uint32_t* idx = nullptr;
	uint32_t first = 0;
	uint32_t* out = gxvk::reserve((uint32_t)count * stride, tris * 3, &idx, &first);
	if (!out)
		return;
	// (a flush in reserve() finished the jobs and let their plans go: the plan again)
	const FetchPlan& plan = planFor(vat);
	const VtxFormat& f = plan.f;
	if (cpuXf)
		for (int i = 0; i < count; i++)
		{
			uint32_t* v = out + (size_t)i * stride;
			v[0] = layout;
			transform(data + (size_t)i * f.size, f, numTex, v + 1);
		}
	else
	{
		uint32_t xfIndex = 0;
		if (uint32_t* x = gxvk::xfBlock(s_xfDirty, &xfIndex))
			fillXf(x);
		s_xfDirty = false;
		const uint32_t meta = layout | xfIndex;
		static const bool slowFetch = getenv("SSA_GX_SLOWFETCH") && atoi(getenv("SSA_GX_SLOWFETCH"));
		if (slowFetch || s_rec)
			for (int i = 0; i < count; i++)
				fetch(data + (size_t)i * f.size, f, meta, out + (size_t)i * stride);
		else
			fetchOrQueue(plan, data, count, meta, stride, out);
	}
	// (SSA_GX_TRACEDRAW=n: draw n of every frame's first vertices as fetched, word by word)
	static const uint64_t traceDraw = getenv("SSA_GX_TRACEDRAW") ? strtoull(getenv("SSA_GX_TRACEDRAW"), nullptr, 0) : 0;
	if (traceDraw && s_frameStats.draws == traceDraw)
	{
		gxFetchFinish();
		fprintf(stderr, "gx: draw %llu fetched (stride %u, layout %08x):", (unsigned long long)traceDraw, stride, layout);
		for (uint32_t i = 0; i < std::min<uint32_t>(2 * stride, 40); i++)
		{
			float fv;
			memcpy(&fv, &out[i], 4);
			fprintf(stderr, " %08x(%g)", out[i], fv);
		}
		fprintf(stderr, "\n");
	}
	// (time-stamp ticks, made ms for the logs: a clock read here cost two system calls a draw on the PS4)
	const uint64_t ticks = __builtin_ia32_rdtsc() - fetchT0;
	s_ticksFetch += ticks;
	s_stats.vertexNs += ticks;
	s_frameStats.vertexNs += ticks;
	uint32_t* p = idx;
	auto tri = [&](int a, int b, int c) {
		*p++ = first + a * stride;
		*p++ = first + b * stride;
		*p++ = first + c * stride;
	};
	switch (primitive)
	{
	case 0:
	case 1:
		for (int i = 0; i + 3 < count; i += 4)
		{
			tri(i, i + 1, i + 2);
			tri(i, i + 2, i + 3);
		}
		break;
	case 2:
		for (int i = 0; i + 2 < count; i += 3)
			tri(i, i + 1, i + 2);
		break;
	case 3:
		for (int i = 0; i + 2 < count; i++)
			if (i & 1)
				tri(i + 1, i, i + 2);
			else
				tri(i, i + 1, i + 2);
		break;
	case 4:
		for (int i = 1; i + 1 < count; i++)
			tri(0, i, i + 1);
		break;
	}
	s_frameStats.triangles += tris;
	const uint64_t drawT0 = __builtin_ia32_rdtsc();
	gxvk::draw((uint32_t)numTex, tris * 3);
	s_ticksDraw += __builtin_ia32_rdtsc() - drawT0;
}

// the EFB copy (BP 0x52): to the XFB ends a frame
void efbCopy(uint32_t value)
{
	const bool toXfb = value & (1u << 14);
	const uint64_t t0 = __builtin_ia32_rdtsc();
	if (!toXfb)
	{
		gxvk::efbCopy("");
		s_ticksCopy += __builtin_ia32_rdtsc() - t0;
		return;
	}
	struct FrameTimer
	{
		uint64_t t0;
		~FrameTimer() { s_ticksFrame += __builtin_ia32_rdtsc() - t0; }
	} frameTimer{t0};
	s_stats.frames++;
	if (getenv("SSA_GX_LOG") && s_stats.frames % 150 == 0)
		fprintf(stderr, "gx: frame %llu: %llu draws, %llu vertices, %llu XF loads, %llu BP, %llu CP, %llu display lists\n",
			(unsigned long long)s_stats.frames, (unsigned long long)s_frameStats.draws, (unsigned long long)s_frameStats.vertices,
			(unsigned long long)s_frameStats.xfLoads, (unsigned long long)s_frameStats.bpWrites, (unsigned long long)s_frameStats.cpWrites,
			(unsigned long long)s_frameStats.displayLists);
	// a captured frame ends at the first XFB copy with something drawn
	std::string path;
	if (s_capturing && s_frameStats.draws && gxvk::rendering())
	{
		char buf[512];
		snprintf(buf, sizeof(buf), "%s/frame%05llu.png", s_frameDir.c_str(), (unsigned long long)s_stats.frames);
		path = buf;
	}
	if (gxvk::efbCopy(path))
	{
		s_framesWritten++;
		s_capturing = false;
		if (getenv("SSA_GX_LOG"))
			fprintf(stderr, "gx: frame %llu written: %llu draws, %llu triangles; positions: %llu skip-index, %llu unmapped; %llu vertices with a matrix index (%llu not a multiple of 3); %llu vertices, %.2f ms in the vertex loops\n",
				(unsigned long long)s_stats.frames, (unsigned long long)s_frameStats.draws, (unsigned long long)s_frameStats.triangles,
				(unsigned long long)s_frameStats.posSkip, (unsigned long long)s_frameStats.posNull, (unsigned long long)s_frameStats.pnmtxVerts,
				(unsigned long long)s_frameStats.pnmtxOdd, (unsigned long long)s_frameStats.vertices, tscMs(s_frameStats.vertexNs));
	}
	if (s_recState == kRecArmed && s_frameStats.draws >= s_recDraws && s_frameStats.draws <= s_recMaxDraws && s_stats.frames >= s_recAt)
		s_recState = kRecStart;
	else if (s_recState == kRecOn && ++s_recFrames >= s_recWanted)
		s_recState = kRecStop;
	if (s_frameStats.draws || !s_capturing)
		s_frameStats = {};
	// the next frame: captured every s_frameEvery-th, drawn also for the two before it (render
	// targets made in earlier frames)
	const bool more = !s_frameDir.empty() && s_framesWritten < s_frameLimit && s_stats.frames + 2 >= s_frameFrom;
	if (!s_capturing && more && s_stats.frames % s_frameEvery == 0)
		s_capturing = true;
	const uint64_t phase = (s_stats.frames + 2) % s_frameEvery;
	gxvk::setRendering(s_renderAll || s_capturing || (more && s_frameEvery > 3 && phase < 2) || s_recState == kRecStart || s_recState == kRecOn);
}

// ---- registers ----
void loadCp(uint8_t reg, uint32_t value)
{
	s_frameStats.cpWrites++;
	s_cp[reg] = value;
	if (reg == 0x30 || reg == 0x40)
		s_xfDirty = true;
}

void loadXf(uint32_t address, const uint8_t* values, int count)
{
	s_frameStats.xfLoads++;
	// (SSA_GX_XFWATCH=address: loads that write it, with the first values)
	static const uint32_t watch = getenv("SSA_GX_XFWATCH") ? (uint32_t)strtoul(getenv("SSA_GX_XFWATCH"), nullptr, 16) : ~0u;
	if (watch != ~0u && watch >= address && watch < address + (uint32_t)count)
	{
		static int logged = 0;
		if (logged++ < 40)
			fprintf(stderr, "gx: frame %llu draw %llu: XF load %04x+%d over %04x: %08x %08x %08x %08x\n", (unsigned long long)s_stats.frames,
				(unsigned long long)s_frameStats.draws, address, count, watch, be32(values + 4 * (watch - address)),
				count > 1 ? be32(values + 4 * std::min<int>(count - 1, (int)(watch - address) + 1)) : 0, be32(values), be32(values + 4 * (count - 1)));
	}
	for (int i = 0; i < count; i++)
	{
		const uint32_t a = address + i;
		if (a < 0x1100)
			s_xf[a] = be32(values + 4 * i);
		s_xfDirty = true;
	}
}

int32_t signed11(uint32_t v)
{
	return (int32_t)(v << 21) >> 21;
}

void loadBp(uint32_t value)
{
	s_frameStats.bpWrites++;
	g_gxStateVersion++;
	const uint8_t reg = value >> 24;
	// (a BP write may be masked by BP 0xFE, which applies to the next write)
	uint32_t v = value & 0xFFFFFF;
	if (reg != 0xFE)
	{
		const uint32_t mask = s_bp[0xFE] ? s_bp[0xFE] & 0xFFFFFF : 0xFFFFFF;
		v = (s_bp[reg] & ~mask) | (v & mask);
		s_bp[0xFE] = 0;
	}
	s_bp[reg] = v;
	if (reg == 0x59)
		s_xfDirty = true;
	// (with the GX thread: the pixel engine's draw-done (0x45) and tokens (0x47; 0x48 with its
	// interrupt) reach the game when decoded, mmio.cpp gxApplyPeEvents)
	if (s_threadMode > 0)
	{
		if (reg == 0x45)
			s_peDone.fetch_add(1, std::memory_order_release);
		else if (reg == 0x47 || reg == 0x48)
		{
			s_peToken.store(v & 0xFFFF, std::memory_order_relaxed);
			s_peTokenSeq.fetch_add(1, std::memory_order_release);
			if (reg == 0x48)
				s_peTokenInts.fetch_add(1, std::memory_order_release);
		}
	}
	// (texture preloads into TMEM: BP 0x60 source address, 0x61 TMEM address, 0x62 count (the write
	// that starts the load), 0x63 TMEM for odd lines; not modeled yet: the first ones logged)
	if (reg >= 0x60 && reg <= 0x63)
	{
		static int s_preloads = 0;
		if (s_preloads++ < 12)
			fprintf(stderr, "gx: TMEM preload register %02x = %06x (0x60 %06x 0x61 %06x)\n", reg, v & 0xFFFFFF, s_bp[0x60] & 0xFFFFFF,
				s_bp[0x61] & 0xFFFFFF);
	}
	if (reg == 0x52)
		efbCopy(v);
	else if (reg == 0x65)
	{
		// TLUT load: from RAM (BP 0x64: address >> 5) to TMEM (bits 0-9: offset >> 9), bits 10-20: 32-byte lines
		const uint32_t from = (s_bp[0x64] & 0xFFFFFF) << 5, to = (v & 0x3FF) << 9, bytes = ((v >> 10) & 0x7FF) * 32;
		uint32_t avail = 0;
		if (const uint8_t* p = readRam(from, bytes, &avail))
			memcpy(&s_tmem[to & 0xFFFFF], p, std::min({bytes, avail, 0x100000u - (to & 0xFFFFF)}));
	}
	else if (reg >= 0xE0 && reg <= 0xE7)
	{
		// TEV registers: RA (red bits 0-10, alpha 12-22), BG (blue, green); bit 23: konst
		const int n = (reg - 0xE0) >> 1;
		int32_t* r = (v >> 23) & 1 ? s_tevKonst[n] : s_tevColor[n];
		if (!(reg & 1))
		{
			r[0] = signed11(v);
			r[3] = signed11(v >> 12);
		}
		else
		{
			r[2] = signed11(v);
			r[1] = signed11(v >> 12);
		}
	}
}

// indexed XF loads (0x20/0x28/0x30/0x38: arrays 12-15): address bits 0-11, size-1 bits 12-15, index 16-31
void loadIndexedXf(int array, uint32_t value)
{
	const uint32_t address = value & 0xFFF, size = ((value >> 12) & 15) + 1, index = value >> 16;
	const uint8_t* p = readRam(s_cp[0xA0 + array] + index * (s_cp[0xB0 + array] & 0xFF), size * 4);
	static const uint32_t watch = getenv("SSA_GX_XFWATCH") ? (uint32_t)strtoul(getenv("SSA_GX_XFWATCH"), nullptr, 16) : ~0u;
	if (watch != ~0u && watch >= address && watch < address + size)
		fprintf(stderr, "gx: indexed XF load (array %d base %08x stride %u, index %u) to %03x+%u\n", array, s_cp[0xA0 + array], s_cp[0xB0 + array] & 0xFF, index, address,
			size);
	if (p)
		loadXf(address, p, (int)size);
}

// ---- the decoder: one command at p (n bytes there); returns its size, 0 if incomplete ----
size_t decodeOne(const uint8_t* p, size_t n, int depth);

// (for the unknown-opcode log: the display list being run, where in it, the command before)
uint32_t s_dlAddress = 0, s_dlSize = 0, s_dlAt = 0, s_prevOp = 0, s_prevSize = 0;
const uint8_t* s_dlStart = nullptr;

void callDisplayList(uint32_t address, uint32_t size, int depth)
{
	s_frameStats.displayLists++;
	const uint8_t* p = size <= 0x400000 ? readRam(address, size) : nullptr;
	if (!p || size > 0x400000 || depth > 4)
		return;
	const uint32_t outerAddress = s_dlAddress, outerSize = s_dlSize;
	const uint8_t* outerStart = s_dlStart;
	s_dlAddress = address;
	s_dlSize = size;
	s_dlStart = p;
	size_t at = 0;
	while (at < size)
	{
		s_dlAt = (uint32_t)at;
		const size_t used = decodeOne(p + at, size - at, depth + 1);
		if (!used)
			break;
		s_prevOp = p[at];
		s_prevSize = (uint32_t)used;
		at += used;
	}
	s_dlAddress = outerAddress;
	s_dlSize = outerSize;
	s_dlStart = outerStart;
}

size_t decodeOne(const uint8_t* p, size_t n, int depth)
{
	if (n < 1)
		return 0;
	const uint8_t op = p[0];
	switch (op)
	{
	case 0x00: // NOP
		return 1;
	case 0x08: // LOAD_CP_REG
		if (n < 6)
			return 0;
		loadCp(p[1], be32(p + 2));
		break;
	case 0x10: // LOAD_XF_REG
	{
		if (n < 5)
			return 0;
		const uint32_t header = be32(p + 1);
		// (1-16 words: bits 16-19, as the hardware and Dolphin take it; the 16 bits taken before let
		// a header with more bits set write the following FIFO bytes over all of XF memory: the
		// identity texture matrix at 0xF0 went, and with it the gameplay's ground textures)
		const int count = (int)((header >> 16) & 15) + 1;
		const size_t size = 5 + 4 * (size_t)count;
		if (n < size)
			return 0;
		loadXf(header & 0xFFFF, p + 5, count);
		s_stats.commands++;
		return size;
	}
	case 0x20: case 0x28: case 0x30: case 0x38: // LOAD_INDX_A-D
		if (n < 5)
			return 0;
		loadIndexedXf(12 + ((op >> 3) & 3), be32(p + 1));
		s_stats.commands++;
		return 5;
	case 0x40: // CALL_DL
		if (n < 9)
			return 0;
		callDisplayList(be32(p + 1), be32(p + 5), depth);
		s_stats.commands++;
		return 9;
	case 0x44: // (unknown; 1 byte in Dolphin)
	case 0x48: // INVALIDATE_VTX_CACHE
		s_stats.commands++;
		return 1;
	case 0x61: // LOAD_BP_REG
		if (n < 5)
			return 0;
		loadBp(be32(p + 1));
		s_stats.commands++;
		return 5;
	default:
		if (op & 0x80)
		{
			// draw: primitive bits 3-6, vertex format bits 0-2, then a 16-bit count and the vertices
			if (n < 3)
				return 0;
			const int vat = op & 7, primitive = (op >> 3) & 15;
			const int count = be16(p + 1);
			const size_t size = 3 + (size_t)count * cachedFormat(vat).size;
			if (n < size)
				return 0;
			draw(primitive, vat, p + 3, count);
			s_stats.commands++;
			return size;
		}
		s_stats.unknown++;
		if (s_stats.unknown <= 20)
		{
			// (in a display list: which, where, the command before it and the bytes from here)
			fprintf(stderr, "gx: unknown opcode %02x (depth %d, frame %llu, draw %llu)", op, depth, (unsigned long long)s_stats.frames,
				(unsigned long long)s_frameStats.draws);
			if (depth > 0)
				fprintf(stderr, " in the display list at %08x+%u, offset %u, after opcode %02x (%u bytes)", s_dlAddress, s_dlSize, s_dlAt, s_prevOp, s_prevSize);
			if ((s_prevOp & 0x80) && depth > 0)
			{
				const int pv = s_prevOp & 7;
				fprintf(stderr, " [VCD %08x %08x VAT%d %08x %08x %08x: vertex %d bytes]", s_cp[0x50], s_cp[0x60], pv, s_cp[0x70 + pv], s_cp[0x80 + pv],
					s_cp[0x90 + pv], vertexFormat(pv).size);
			}
			fprintf(stderr, "; bytes:");
			for (size_t i = 0; i < std::min<size_t>(n, 24); i++)
				fprintf(stderr, " %02x", p[i]);
			if (depth > 0 && s_dlStart && s_dlAt >= s_prevSize)
			{
				fprintf(stderr, "; the command before:");
				for (uint32_t i = 0; i < std::min<uint32_t>(s_prevSize, 24); i++)
					fprintf(stderr, " %02x", s_dlStart[s_dlAt - s_prevSize + i]);
			}
			fprintf(stderr, "\n");
		}
		return 1;
	}
	s_stats.commands++;
	return 6;
}
} // namespace

// ---- for the backend ----
void gxWritePng(const std::string& path, int w, int h, const uint32_t* argbPixels)
{
	std::vector<uint8_t> raw;
	raw.reserve((size_t)(w * 3 + 1) * h);
	for (int y = 0; y < h; y++)
	{
		raw.push_back(0);
		for (int x = 0; x < w; x++)
		{
			const uint32_t c = argbPixels[y * w + x];
			raw.push_back((uint8_t)(c >> 16));
			raw.push_back((uint8_t)(c >> 8));
			raw.push_back((uint8_t)c);
		}
	}
	uLongf zlen = compressBound(raw.size());
	std::vector<uint8_t> z(zlen);
	compress2(z.data(), &zlen, raw.data(), raw.size(), 6);
	z.resize(zlen);
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
		(uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
	pngChunk(f, "IHDR", ihdr);
	pngChunk(f, "IDAT", z);
	pngChunk(f, "IEND", {});
	fclose(f);
}

const uint8_t* gxPhys(uint32_t address, uint32_t* avail)
{
	return phys(address, avail);
}
const uint8_t* gxReadRam(uint32_t address, uint32_t len, uint32_t* avail)
{
	return readRam(address, len, avail);
}
const uint8_t* gxTmem()
{
	return s_tmem.data();
}
const uint32_t* gxBp()
{
	return s_bp;
}
const int32_t* gxTevColor()
{
	return &s_tevColor[0][0];
}
const int32_t* gxTevKonst()
{
	return &s_tevKonst[0][0];
}

uint32_t gxTextureBytes(int fmt, int w, int h)
{
	int tw, th, bpp;
	if (!formatTiles(fmt, tw, th, bpp))
		return 0;
	return (uint32_t)(((w + tw - 1) / tw) * ((h + th - 1) / th) * (tw * th * bpp / 8));
}

// map n's texture (BP MODE0 0x80/0xA0, IMAGE0 0x88/0xA8, IMAGE3 0x94/0xB4, TLUT 0x98/0xB8)
bool gxTextureInfo(int map, GxTextureInfo& ti)
{
	const int i = map & 3, hi = map >= 4;
	ti.image0 = s_bp[(hi ? 0xA8 : 0x88) + i];
	ti.tlut = s_bp[(hi ? 0xB8 : 0x98) + i];
	ti.mode0 = s_bp[(hi ? 0xA0 : 0x80) + i];
	ti.w = (int)(ti.image0 & 0x3FF) + 1;
	ti.h = (int)((ti.image0 >> 10) & 0x3FF) + 1;
	ti.fmt = (int)((ti.image0 >> 20) & 15);
	ti.address = (s_bp[(hi ? 0xB4 : 0x94) + i] & 0xFFFFFF) << 5;
	ti.bytes = gxTextureBytes(ti.fmt, ti.w, ti.h);
	if (!ti.bytes)
		return false;
	uint32_t avail = 0;
	if (!phys(ti.address, &avail) || ti.bytes > avail)
		return false;
	ti.tlutOffset = (ti.tlut & 0x3FF) << 9;
	ti.tlutBytes = ti.fmt == 8 ? 32 : ti.fmt == 9 ? 512 : ti.fmt == 10 ? 32768 : 0;
	return true;
}

// map n's texture, level 0, decoded from its tiles
bool gxDecodeTexture(int map, std::vector<uint32_t>& px)
{
	GxTextureInfo ti;
	if (!gxTextureInfo(map, ti))
		return false;
	const int w = ti.w, h = ti.h, fmt = ti.fmt;
	const uint32_t tlutAddr = ti.tlutOffset, tlutFmt = (ti.tlut >> 10) & 3;
	int tw, th, bpp;
	formatTiles(fmt, tw, th, bpp);
	const int tilesX = (w + tw - 1) / tw, tilesY = (h + th - 1) / th;
	const uint32_t tileBytes = (uint32_t)(tw * th * bpp / 8);
	const uint8_t* src = readRam(ti.address, ti.bytes);
	px.assign((size_t)w * h, 0);
	auto put = [&](int x, int y, uint32_t c) {
		if (x < w && y < h)
			px[(size_t)y * w + x] = c;
	};
	for (int ty = 0; ty < tilesY; ty++)
		for (int tx = 0; tx < tilesX; tx++)
		{
			const uint8_t* tile = src + ((size_t)ty * tilesX + tx) * tileBytes;
			const int x0 = tx * tw, y0 = ty * th;
			if (fmt == 14)
			{
				// CMPR: 2x2 DXT1 blocks (big-endian colors, indices 2 bits MSB first)
				for (int b = 0; b < 4; b++)
				{
					const uint8_t* blk = tile + b * 8;
					const uint16_t c0 = be16(blk), c1 = be16(blk + 2);
					uint32_t pal[4] = {from565(c0), from565(c1), 0, 0};
					auto mix = [](uint32_t a, uint32_t b, int wa, int wb, int d) {
						uint32_t r = 0xFF000000;
						for (int s = 0; s < 24; s += 8)
							r |= ((((a >> s) & 0xFF) * wa + ((b >> s) & 0xFF) * wb) / d) << s;
						return r;
					};
					if (c0 > c1)
					{
						pal[2] = mix(pal[0], pal[1], 2, 1, 3);
						pal[3] = mix(pal[0], pal[1], 1, 2, 3);
					}
					else
					{
						pal[2] = mix(pal[0], pal[1], 1, 1, 2);
						pal[3] = 0; // transparent
					}
					const int bx = x0 + (b & 1) * 4, by = y0 + (b >> 1) * 4;
					for (int y = 0; y < 4; y++)
						for (int x = 0; x < 4; x++)
							put(bx + x, by + y, pal[(blk[4 + y] >> (6 - 2 * x)) & 3]);
				}
				continue;
			}
			for (int y = 0; y < th; y++)
				for (int x = 0; x < tw; x++)
				{
					const int n = y * tw + x;
					uint32_t c = 0;
					switch (fmt)
					{
					case 0: { const uint32_t v = ((tile[n / 2] >> ((n & 1) ? 0 : 4)) & 15) * 17; c = argb(v, v, v, v); break; }
					case 1: { const uint32_t v = tile[n]; c = argb(v, v, v, v); break; }
					case 2: { const uint32_t a = (tile[n] >> 4) * 17, v = (tile[n] & 15) * 17; c = argb(a, v, v, v); break; }
					case 3: { const uint32_t a = tile[2 * n], v = tile[2 * n + 1]; c = argb(a, v, v, v); break; }
					case 4: c = from565(be16(tile + 2 * n)); break;
					case 5: c = from5A3(be16(tile + 2 * n)); break;
					case 6: c = argb(tile[2 * n], tile[2 * n + 1], tile[32 + 2 * n], tile[32 + 2 * n + 1]); break;
					case 8: c = tlutColor(tlutAddr, tlutFmt, (tile[n / 2] >> ((n & 1) ? 0 : 4)) & 15); break;
					case 9: c = tlutColor(tlutAddr, tlutFmt, tile[n]); break;
					case 10: c = tlutColor(tlutAddr, tlutFmt, be16(tile + 2 * n) & 0x3FFF); break;
					}
					put(x0 + x, y0 + y, c);
				}
		}
	return true;
}

namespace
{
bool s_syncing = false;
std::vector<uint8_t> s_unrecorded; // FIFO bytes not yet in the recording
} // namespace

// time-stamp-counter ticks spent in the renderer (the decode and draw recording, and of that the
// wait for the GPU), for the runtime's per-second time split (ssa_run.cpp onSecond)
volatile uint64_t g_ssaTicksGx = 0, g_ssaTicksGpuWait = 0;
uint64_t gxFrameCount()
{
	return s_stats.frames;
}
namespace
{
struct GxTimer
{
	const uint64_t t0 = __builtin_ia32_rdtsc();
	~GxTimer() { g_ssaTicksGx = g_ssaTicksGx + (__builtin_ia32_rdtsc() - t0); }
};
} // namespace

// the FIFO's bytes, as the command processor reads them (32 at a time): decoded and drawn (on the GX
// thread when it runs)
static void fifoDecode(const uint8_t* data, size_t size)
{
	const GxTimer timer;
	static bool init = false;
	if (!init)
	{
		init = true;
		if (const char* d = getenv("SSA_FRAMES"))
			s_frameDir = d;
		if (const char* e = getenv("SSA_FRAMES_EVERY"))
			s_frameEvery = std::max(1, atoi(e));
		if (const char* e = getenv("SSA_FRAMES_FROM"))
			s_frameFrom = strtoull(e, nullptr, 0);
		s_renderAll = getenv("SSA_VK_ALL") != nullptr;
		if (s_renderAll || !s_frameDir.empty())
			gxvk::init();
		gxvk::setRendering(s_renderAll);
		if (getenv("SSA_GXREC"))
		{
			s_recState = kRecArmed;
			if (const char* e = getenv("SSA_GXREC_DRAWS"))
				s_recDraws = strtoull(e, nullptr, 0);
			if (const char* e = getenv("SSA_GXREC_FRAMES"))
				s_recWanted = std::max(1, atoi(e));
			if (const char* e = getenv("SSA_GXREC_AT"))
				s_recAt = strtoull(e, nullptr, 0);
			if (const char* e = getenv("SSA_GXREC_MAXDRAWS"))
				s_recMaxDraws = strtoull(e, nullptr, 0);
		}
	}
	if (s_recState == kRecStart)
		recStart();
	else if (s_recState == kRecStop)
		recStop();
	s_pending.insert(s_pending.end(), data, data + size);
	if (s_unrecordedClear)
	{
		s_unrecordedClear = false;
		s_unrecorded.clear();
	}
	if (s_rec)
		s_unrecorded.insert(s_unrecorded.end(), data, data + size);
	// (SSA_GX_DEFER=1: the bytes wait for the game's next sync point (gxSync) or 4 MB, as the GPU reads
	// vertex arrays and display lists only when it gets to them)
	static const bool defer = getenv("SSA_GX_DEFER") && atoi(getenv("SSA_GX_DEFER"));
	if (defer && !s_syncing && s_pending.size() < (4u << 20))
		return;
	size_t at = 0;
	while (at < s_pending.size())
	{
		const size_t used = decodeOne(s_pending.data() + at, s_pending.size() - at, 0);
		if (!used)
			break;
		at += used;
	}
	s_pending.erase(s_pending.begin(), s_pending.begin() + at);
	if (s_rec)
	{
		// (the RAM the decode read goes before the bytes it decoded, so a replay, which decodes at
		// once, has it in time)
		if (!s_patches.empty())
			gzwrite(s_rec, s_patches.data(), (unsigned)s_patches.size());
		s_patches.clear();
		if (!s_unrecorded.empty())
		{
			const uint8_t type = 1;
			const uint32_t n = (uint32_t)s_unrecorded.size();
			gzwrite(s_rec, &type, 1);
			gzwrite(s_rec, &n, 4);
			gzwrite(s_rec, s_unrecorded.data(), n);
			s_unrecorded.clear();
		}
	}
}

// ---- the GX thread (SSA_GX_THREAD=1; the PS4's default) ----
// The decode, the draws' recording and the presentation run on a thread of their own, as the real GPU
// runs beside the CPU (Dolphin's dual core): the command processor's bytes go through a single-producer
// ring (the game thread copies each 32-byte block in; the GX thread takes what is there). The game sees
// the GPU's progress only as far as the GX thread has got: the CP breakpoint counts as reached, and the
// pixel engine's draw-done and tokens are set, once their bytes are decoded (mmio.cpp asks with
// gxFifoPosition / gxFifoReached / gxPeEvents). Not with a recording (SSA_GXREC).
namespace
{
constexpr size_t kRingBytes = 16u << 20;
uint8_t* s_ring = nullptr;
std::atomic<uint64_t> s_ringHead{0}, s_ringTail{0}; // bytes put in / decoded, since the start
uint64_t s_producerWaits = 0;

void waitBriefly(int& spins)
{
	if (++spins < 200)
		__builtin_ia32_pause();
	else if (spins < 400)
		std::this_thread::yield();
	else
		std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void gxThreadMain()
{
	// (the PC host's once-a-second SIGALRM goes to the game thread: its handler logs, and run on this
	// thread inside a lock the renderer holds it deadlocked; the Vulkan driver's threads, made from
	// this one, inherit the mask)
	sigset_t block;
	sigemptyset(&block);
	sigaddset(&block, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &block, nullptr);
	if (g_ssaPinThread)
		g_ssaPinThread(1, 650, "gx");
	uint64_t tail = s_ringTail.load();
	uint64_t reportAt = 0;
	for (;;)
	{
		int spins = 0;
		uint64_t head;
		while ((head = s_ringHead.load(std::memory_order_acquire)) == tail)
		{
			// (the game paused for the quick menu, everything sent drawn: the last frame again with the
			// menu over it, paced by the swapchain)
			if (spins >= 400 && g_ssaPaused.load(std::memory_order_relaxed))
			{
				if (!gxvk::presentIdle())
					std::this_thread::sleep_for(std::chrono::milliseconds(8));
				continue;
			}
			waitBriefly(spins);
		}
		// every 10 s: where the GX thread's time went (SSA_GX_TIMES=0: off)
		static const bool times = !getenv("SSA_GX_TIMES") || atoi(getenv("SSA_GX_TIMES"));
		const uint64_t now = __builtin_ia32_rdtsc();
		if (times && now >= reportAt)
		{
			const double tick = tscPerNs() * 1e7; // (ticks per 1% of 1 s)
			static uint64_t last = 0, lBusy = 0, lFetch = 0, lTex = 0, lDraw = 0, lCopy = 0, lFrame = 0, lWait = 0, lFinish = 0, lFrames = 0;
			if (last)
			{
				const double span = (double)(now - last) / tick; // (percent units: 100 = one second)
				auto pct = [&](uint64_t v) { return (double)v / tick / span * 100.0; };
				fprintf(stderr, "gx: GX thread %.0f s: busy %.0f%% (vertices %.0f%%, draws %.0f%% (textures %.0f%%), EFB copies %.0f%%, frame ends %.0f%%, "
					"waits for the GPU %.0f%%, for fetch workers %.0f%%), %.1f frames/s\n",
					span / 100.0, pct(g_ssaTicksGx - lBusy), pct(s_ticksFetch - lFetch), pct(s_ticksDraw - lDraw), pct(g_gxTicksTextures - lTex),
					pct(s_ticksCopy - lCopy), pct(s_ticksFrame - lFrame), pct(g_ssaTicksGpuWait - lWait), pct(s_ticksFinish - lFinish),
					(double)(s_stats.frames - lFrames) / (span / 100.0));
			}
			last = now, lBusy = g_ssaTicksGx, lFetch = s_ticksFetch, lTex = g_gxTicksTextures, lDraw = s_ticksDraw, lCopy = s_ticksCopy, lFrame = s_ticksFrame,
			lWait = g_ssaTicksGpuWait, lFinish = s_ticksFinish, lFrames = s_stats.frames;
			reportAt = now + (uint64_t)(tick * 1000);
		}
		// (up to the ring's end; the rest next time)
		const size_t at = (size_t)(tail % kRingBytes);
		const size_t n = (size_t)std::min<uint64_t>(head - tail, kRingBytes - at);
		fifoDecode(s_ring + at, n);
		tail += n;
		s_ringTail.store(tail, std::memory_order_release);
	}
}
} // namespace

bool gxThreaded()
{
	if (s_threadMode < 0)
	{
		const char* e = getenv("SSA_GX_THREAD");
		s_threadMode = e && atoi(e) && !getenv("SSA_GXREC") ? 1 : 0;
		if (s_threadMode)
		{
			s_ring = (uint8_t*)malloc(kRingBytes);
			if (!s_ring)
				s_threadMode = 0;
			else
			{
				fprintf(stderr, "gx: the GX thread decodes and draws (SSA_GX_THREAD=0: on the game thread)\n");
				std::thread(gxThreadMain).detach();
			}
		}
	}
	return s_threadMode > 0;
}

// the game paused with no GX thread (SSA_GX_THREAD=0): the pause loop's present (ssa_run.cpp)
void gxIdlePresent()
{
	if (gxThreaded() || !gxvk::presentIdle())
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
}

// the FIFO's bytes, as the command processor reads them
void gxFifoData(const uint8_t* data, size_t size)
{
	if (!gxThreaded())
	{
		fifoDecode(data, size);
		return;
	}
	if (!size)
		return;
	const uint64_t head = s_ringHead.load(std::memory_order_relaxed);
	// (the ring full: the GX thread is behind by 16 MB; wait for it)
	int spins = 0;
	if (head + size - s_ringTail.load(std::memory_order_acquire) > kRingBytes)
	{
		s_producerWaits++;
		while (head + size - s_ringTail.load(std::memory_order_acquire) > kRingBytes)
			waitBriefly(spins);
	}
	const size_t at = (size_t)(head % kRingBytes);
	const size_t first = std::min(size, kRingBytes - at);
	memcpy(s_ring + at, data, first);
	if (first < size)
		memcpy(s_ring, data + first, size - first);
	s_ringHead.store(head + size, std::memory_order_release);
}

// where the FIFO's bytes have got to (a marker for gxFifoReached), and whether the GX thread has
// decoded that far
uint64_t gxFifoPosition()
{
	return s_ringHead.load(std::memory_order_relaxed);
}
bool gxFifoReached(uint64_t position)
{
	return !gxThreaded() || s_ringTail.load(std::memory_order_acquire) >= position;
}

// the pixel engine's events the GX thread has decoded (counts since the start; the last token)
void gxPeEvents(uint32_t* done, uint32_t* tokenInts, uint32_t* tokenSeq, uint32_t* token)
{
	*done = s_peDone.load(std::memory_order_acquire);
	*tokenInts = s_peTokenInts.load(std::memory_order_acquire);
	*tokenSeq = s_peTokenSeq.load(std::memory_order_acquire);
	*token = s_peToken.load(std::memory_order_acquire);
}

// a sync point: everything in the FIFO is drawn
void gxSync()
{
	if (gxThreaded())
	{
		const uint64_t head = s_ringHead.load(std::memory_order_relaxed);
		int spins = 0;
		while (s_ringTail.load(std::memory_order_acquire) < head)
			waitBriefly(spins);
		return;
	}
	if (s_pending.empty())
		return;
	s_syncing = true;
	fifoDecode(nullptr, 0);
	s_syncing = false;
}

// a recording (SSA_GXREC) drawn again: state and RAM, then the records in order
int gxReplay(const char* path)
{
	gzFile f = gzopen(path, "rb");
	if (!f)
		return 1;
	gzbuffer(f, 1 << 20);
	char magic[4];
	if (gzread(f, magic, 4) != 4 || memcmp(magic, "GXR1", 4))
		return 2;
	gzread(f, s_cp, sizeof(s_cp));
	gzread(f, s_xf, sizeof(s_xf));
	s_xfDirty = true;
	gzread(f, s_bp, sizeof(s_bp));
	gzread(f, s_tevColor, sizeof(s_tevColor));
	gzread(f, s_tevKonst, sizeof(s_tevKonst));
	gzread(f, s_tmem.data(), (unsigned)s_tmem.size());
	uint32_t pending = 0;
	gzread(f, &pending, 4);
	s_pending.resize(pending);
	if (pending)
		gzread(f, s_pending.data(), pending);
	gzread(f, memory_base + 0x80000000u, kMem1);
	gzread(f, memory_base + 0x90000000u, kMem2);
	std::vector<uint8_t> buf;
	uint64_t fifo = 0, patches = 0;
	for (;;)
	{
		uint8_t type = 0;
		if (gzread(f, &type, 1) != 1 || type == 0)
			break;
		if (type == 2)
		{
			uint32_t addr = 0, n = 0, avail = 0;
			gzread(f, &addr, 4);
			gzread(f, &n, 4);
			uint8_t* p = (uint8_t*)phys(addr, &avail);
			buf.resize(n);
			gzread(f, buf.data(), n);
			if (p)
				memcpy(p, buf.data(), std::min(n, avail));
			patches++;
		}
		else
		{
			uint32_t n = 0;
			gzread(f, &n, 4);
			buf.resize(n);
			gzread(f, buf.data(), n);
			gxFifoData(buf.data(), n);
			fifo += n;
		}
	}
	gzclose(f);
	fprintf(stderr, "gx: replayed %llu FIFO bytes, %llu RAM records\n", (unsigned long long)fifo, (unsigned long long)patches);
	return 0;
}

// totals for the end-of-run report
void gxReport()
{
	gxSync();
	if (gxThreaded())
		fprintf(stderr, "gx: GX thread: %llu bytes decoded, the game waited for it %llu times (ring full)\n",
			(unsigned long long)s_ringTail.load(), (unsigned long long)s_producerWaits);
	fprintf(stderr, "gx: %llu frames (XFB copies), %llu commands, %llu unknown opcodes, %llu line/point draws (not drawn), %d frames written\n",
		(unsigned long long)s_stats.frames, (unsigned long long)s_stats.commands, (unsigned long long)s_stats.unknown, (unsigned long long)s_stats.lines,
		s_framesWritten);
	fprintf(stderr, "gx: %.1f ms in the vertex loops (%s)\n", tscMs(s_stats.vertexNs), getenv("SSA_GX_CPUXF") && atoi(getenv("SSA_GX_CPUXF")) ? "CPU transform" : "fetch");
	gxvk::report();
}
