// Skylanders runtime: the GX renderer's Vulkan backend (gx_vk.cpp) and what it needs from the
// command stream decoder (gx.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gxvk
{
// the device and the EFB; false: no Vulkan device (nothing is drawn)
bool init();
bool ok();
// drawing on or off from the next draw (the PC host draws only the frames it captures)
void setRendering(bool on);
bool rendering();
// room for a draw: words vertex words and indexCount indices (and an XF block). Returns the
// vertex words; the indices go to *indices and are word offsets: a vertex at returned + k has
// index firstWord + k
uint32_t* reserve(uint32_t words, uint32_t indexCount, uint32_t** indices, uint32_t* firstWord);
// the XF snapshot the next vertices use (kXfWords words, tev.vert's layout): changed (or after a
// flush) a new one, whose words are returned to be filled; else nullptr. *index: its number
constexpr uint32_t kXfWords = 868;
uint32_t* xfBlock(bool changed, uint32_t* index);
// the draw reserved last, with the current BP state
void draw(uint32_t numTex, uint32_t indexCount);
// BP 0x52 written: an EFB copy. To the XFB it ends the frame; capturePath: write it as a PNG
bool efbCopy(const std::string& capturePath); // true: the frame went to capturePath (only XFB copies VI shows)
void report();
// the game paused (the quick menu): the last frame again with the menu over it (on the thread that
// submits frames; paced by the swapchain). False: nothing presented.
bool presentIdle();
} // namespace gxvk

// from the decoder
struct GxTextureInfo
{
	uint32_t address = 0, bytes = 0; // the texels in RAM
	int w = 0, h = 0, fmt = 0;
	uint32_t tlutOffset = 0, tlutBytes = 0; // a palette in TMEM (C4, C8, C14X2)
	uint32_t image0 = 0, tlut = 0, mode0 = 0;
};
bool gxTextureInfo(int map, GxTextureInfo& info);
bool gxDecodeTexture(int map, std::vector<uint32_t>& argb); // level 0, ARGB (0xAARRGGBB)
uint32_t gxTextureBytes(int fmt, int w, int h);
const uint8_t* gxPhys(uint32_t address, uint32_t* avail);
const uint8_t* gxReadRam(uint32_t address, uint32_t len, uint32_t* avail); // (recorded while recording)
const uint8_t* gxTmem();
const uint32_t* gxBp();
const int32_t* gxTevColor(); // [4][4]: PREV, C0, C1, C2 (r, g, b, a)
const int32_t* gxTevKonst(); // [4][4]: K0-K3
void gxWritePng(const std::string& path, int w, int h, const uint32_t* argb);
// the vertex fetch workers have filled every vertex reserved so far (before a submit)
void gxFetchFinish();
// the renderer's time on the GX thread, in time-stamp ticks: the texture lookups (hash, decode, upload)
extern uint64_t g_gxTicksTextures;
// counts BP writes and EFB copies: a draw with the same count as the last one has the same TEV/BP
// state and textures (within a frame)
extern uint32_t g_gxStateVersion;
