// Skylanders runtime: the GX renderer's Vulkan backend (R2). After Dolphin's VideoCommon and
// Vulkan backend in its choices (GPL-2.0-or-later, like the rest of the runtime).
//
// The EFB is a 640x528 BGRA8 + D32 image pair drawn with dynamic rendering. Draws come from the
// decoder (gx.cpp) as fetched vertices (decoded to floats) that tev.vert transforms and lights with
// the XF snapshot (matrices, lights, XF registers: one per change, in a ring) each vertex names;
// with SSA_GX_CPUXF=1 they come already transformed. Vertices and indices go to host-visible rings, the
// draw's TEV/BP state to a storage buffer of 512-byte states (deduplicated), textures to one
// descriptor array (slots) with 36 samplers, and pipelines are made on demand per blend / depth /
// cull state. Consecutive draws with the same pipeline, scissor and state are merged.
//
// EFB copies render the EFB rectangle into a texture (kept by its RAM address; a later texture at
// that address with the same size uses it while the RAM there is unchanged), or into the XFB image,
// which ends the frame: the frame's commands are submitted and waited for (one frame in flight), and
// a captured frame is read back and written as a PNG.
//
// Missing: mipmaps and LOD, the bump alpha, indirect textures, Z textures, lines and points, EFB
// reads and pokes from the CPU, and copies to RAM (for the CPU).
//
// Vulkan functions come from a table (vk_fns.h) loaded through vkGetInstanceProcAddr: the PS4's RADV
// is linked into the app (ps4-kit, no loader). On the PS4 every frame is presented: the XFB is scaled
// into a 1080p swapchain image on the kit's VideoOut surface (4:3 pillarboxed; SSA_WIDE=1 16:9).
#include "gx_vk.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "vk_fns.h"
#ifdef SSA_PS4
#include "ps4_vk_wsi.h"
#else
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name); // libvulkan
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <tuple>

#include "shaders/tev_vert.h"
#include "shaders/tev_frag.h"
#include "shaders/tev_frag_dual.h"
#include "shaders/overlay_vert.h"
#include "shaders/overlay_frag.h"
#include "overlay.h"
#include "settings.h"
#include <imgui.h>
#include "shaders/copy_vert.h"
#include "shaders/copy_frag.h"

__attribute__((weak)) uint32_t mmioRegValue(uint32_t a); // mmio.cpp (absent in gx_replay)
extern volatile uint64_t g_ssaTicksGpuWait; // gx.cpp

namespace
{
#define VKFN_DECL(f) PFN_##f f = nullptr;
VK_FNS_GLOBAL(VKFN_DECL)
VK_FNS(VKFN_DECL)
VK_FNS_PRESENT(VKFN_DECL)
#undef VKFN_DECL
PFN_vkGetInstanceProcAddr s_gipa = nullptr;
// presenting: the swapchain, its images, the image acquired for this frame
bool s_present = false;
VkSurfaceKHR s_surface = VK_NULL_HANDLE;
VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
VkImage s_swapImages[8];
VkImageView s_swapViews[8] = {};
uint32_t s_swapCount = 0, s_swapIndex = ~0u;
VkSemaphore s_acquired = VK_NULL_HANDLE;
bool s_waitAcquired = false;
constexpr uint32_t kScreenW = 1920, kScreenH = 1080;

constexpr int kEfbW = 640, kEfbH = 528;
constexpr int kXfbW = 640, kXfbH = 576;
// the internal resolution: the EFB, the XFB and EFB copies at s_scale times the Wii's pixels (the
// quick menu's setting at start; SSA_VK_SCALE=n replaces it). Everything the game gives in EFB pixels
// (scissors, copy rectangles, clears) is scaled; tev.vert's viewport math is in EFB units already, and
// textures (EFB copies too) are sampled with normalized coordinates.
int s_scale = 1;
constexpr VkFormat kColorFmt = VK_FORMAT_B8G8R8A8_UNORM, kDepthFmt = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kSlots = 8192;
// (whole rings: each frame in flight has half)
constexpr VkDeviceSize kVertBytes = 96u << 20, kIdxBytes = 24u << 20, kStateBytes = 16u << 20, kStagingBytes = 64u << 20;
constexpr VkDeviceSize kXfBytes = (VkDeviceSize)gxvk::kXfWords * 4 * 16384; // (16384 blocks: the vertex's 14-bit index)

struct GpuState
{
	uint32_t genmode, alphaTest, flags, pad0;
	uint32_t tref[4];
	uint32_t cenv[16];
	uint32_t aenv[16];
	uint32_t ksel[8];
	int32_t reg[4][4];
	int32_t konst[4][4];
	uint32_t tex[8];
	float suSize[8][4];
	float fog[4];
	uint32_t fogSel, fogColor, pad1, pad2;
	// indirect texturing: the TEV stages' indirect commands (BP 0x10-0x1F), the indirect stages'
	// maps and coordinates (BP 0x27), their scales (BP 0x25/0x26), and the three matrices as Dolphin's
	// shader takes them: rows (ma mc me | mb md mf), w = 17 - the scale exponent
	uint32_t indCmd[16];
	uint32_t iref, ss0, ss1, pad3;
	int32_t indMtx[6][4];
};
static_assert(sizeof(GpuState) == 688, "the shader's State");

struct PushConstants
{
	uint32_t state, numTex, pad0, pad1;
};

struct Buffer
{
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	uint8_t* map = nullptr;
	VkDeviceSize size = 0;
};

struct Image
{
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	int w = 0, h = 0;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Texture
{
	Image im;
	int slot = -1;
	uint64_t hash = 0;
	uint64_t validFrame = ~0ull, lastUsed = 0;
	uint32_t ramBytes = 0; // (EFB copies: the RAM range whose hash was taken)
};

struct TexKey
{
	uint32_t address, image0, tlut;
	bool operator==(const TexKey& o) const { return address == o.address && image0 == o.image0 && tlut == o.tlut; }
};
struct TexKeyHash
{
	size_t operator()(const TexKey& k) const { return (size_t)k.address * 0x9E3779B97F4A7C15ull ^ (size_t)k.image0 * 0xC2B2AE3D27D4EB4Full ^ k.tlut; }
};

struct PendingDraw
{
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkRect2D scissor{};
	PushConstants pc{};
	uint32_t firstIndex = 0, count = 0;
};

VkInstance s_instance;
VkPhysicalDevice s_phys;
VkDevice s_dev;
VkQueue s_queue;
uint32_t s_queueFamily;
VkPhysicalDeviceMemoryProperties s_memProps;
bool s_logicOp = false;
bool s_dualSrc = false; // dual-source blending (the destination alpha constant on blending draws)
// two frames in flight: each has its own command pool and buffers, fence, half of every ring and its
// retired textures; a frame's end submits it and waits only for the frame before last
constexpr int kInFlight = 2;
VkCommandPool s_pools[kInFlight];
VkCommandBuffer s_uploadCbs[kInFlight], s_cbs[kInFlight];
VkFence s_fences[kInFlight];
bool s_fencePending[kInFlight] = {};
int s_set = 0;
VkCommandBuffer s_uploadCb, s_cb; // (the current frame's)
bool s_ok = false, s_tried = false, s_render = false, s_recording = false, s_inRendering = false, s_uploads = false;
Buffer s_verts, s_idx, s_states, s_xfs, s_staging, s_readback;
// the rings' write positions (absolute: words, indices, states, XF blocks, bytes) and the current
// frame's half of each ([s_set * half, (s_set + 1) * half))
VkDeviceSize s_vertWords = 0, s_idxCount = 0, s_stateCount = 0, s_xfCount = 0, s_stagingUsed = 0;
VkDeviceSize s_vertEnd = 0, s_idxEnd = 0, s_stateEnd = 0, s_xfEnd = 0, s_stagingEnd = 0;
uint32_t s_xfCurrent = ~0u; // (the XF block the vertices use; ~0: none since the last flush)
Image s_efbColor, s_efbDepth, s_xfb;
VkDescriptorSetLayout s_mainLayout, s_copyLayout;
VkPipelineLayout s_mainPL, s_copyPL;
VkDescriptorPool s_dpool;
VkDescriptorSet s_mainSet, s_copySet;
VkSampler s_samplers[64], s_pointSampler;
VkShaderModule s_tevVert, s_tevFrag, s_copyVert, s_copyFrag;
VkPipelineCache s_pipelineCache;
std::unordered_map<uint32_t, VkPipeline> s_pipelines;
VkPipeline s_copyPipeline;
VkPipeline s_boundPipeline = VK_NULL_HANDLE;
std::vector<int> s_freeSlots;
std::unordered_map<TexKey, Texture, TexKeyHash> s_textures;
std::unordered_map<uint32_t, Texture> s_efbCopies;
std::vector<Texture> s_garbage[kInFlight]; // (retired in that frame: destroyed once it has run)
uint64_t s_frame = 0;
PendingDraw s_pending;
GpuState s_lastState;
uint32_t s_lastStateIndex = ~0u;
std::string s_capturePath;
int s_captureW = 0, s_captureH = 0;

struct Stats
{
	uint64_t draws = 0, gpuDraws = 0, states = 0, xfBlocks = 0, pipelines = 0, textures = 0, efbCopies = 0, efbCopyHits = 0, flushes = 0, frames = 0;
} s_stats;

#define VKCHECK(x)                                                                         \
	do                                                                                     \
	{                                                                                      \
		VkResult r_ = (x);                                                                 \
		if (r_ != VK_SUCCESS)                                                              \
		{                                                                                  \
			fprintf(stderr, "gx-vk: %s failed: %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); \
			return false;                                                                  \
		}                                                                                  \
	} while (0)

uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (s_memProps.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return ~0u;
}

bool makeBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = size;
	ci.usage = usage;
	VKCHECK(vkCreateBuffer(s_dev, &ci, nullptr, &b.buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(s_dev, b.buf, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (ai.memoryTypeIndex == ~0u)
		return false;
	VKCHECK(vkAllocateMemory(s_dev, &ai, nullptr, &b.mem));
	VKCHECK(vkBindBufferMemory(s_dev, b.buf, b.mem, 0));
	void* p = nullptr;
	VKCHECK(vkMapMemory(s_dev, b.mem, 0, size, 0, &p));
	b.map = (uint8_t*)p;
	b.size = size;
	return true;
}

bool makeImage(Image& im, int w, int h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
	VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = fmt;
	ci.extent = {(uint32_t)w, (uint32_t)h, 1};
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKCHECK(vkCreateImage(s_dev, &ci, nullptr, &im.img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s_dev, im.img, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == ~0u)
		ai.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
	VKCHECK(vkAllocateMemory(s_dev, &ai, nullptr, &im.mem));
	VKCHECK(vkBindImageMemory(s_dev, im.img, im.mem, 0));
	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = im.img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = fmt;
	vi.subresourceRange = {aspect, 0, 1, 0, 1};
	VKCHECK(vkCreateImageView(s_dev, &vi, nullptr, &im.view));
	im.w = w;
	im.h = h;
	im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

void destroyImage(Image& im)
{
	if (im.view)
		vkDestroyImageView(s_dev, im.view, nullptr);
	if (im.img)
		vkDestroyImage(s_dev, im.img, nullptr);
	if (im.mem)
		vkFreeMemory(s_dev, im.mem, nullptr);
	im = {};
}

// a layout change with the stages that use the image on either side
void transition(VkCommandBuffer cb, Image& im, VkImageLayout to, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
	if (im.layout == to)
		return;
	auto access = [](VkImageLayout l, VkPipelineStageFlags& stage) -> VkAccessFlags {
		switch (l)
		{
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			return VK_ACCESS_SHADER_READ_BIT;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			return VK_ACCESS_TRANSFER_WRITE_BIT;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			return VK_ACCESS_TRANSFER_READ_BIT;
		default:
			stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			return 0;
		}
	};
	VkPipelineStageFlags srcStage, dstStage;
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = access(im.layout, srcStage);
	b.dstAccessMask = access(to, dstStage);
	b.oldLayout = im.layout;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im.img;
	b.subresourceRange = {aspect, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
	im.layout = to;
}

VkShaderModule shader(const uint32_t* code, size_t bytes)
{
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = bytes;
	ci.pCode = code;
	VkShaderModule m = VK_NULL_HANDLE;
	if (vkCreateShaderModule(s_dev, &ci, nullptr, &m) != VK_SUCCESS)
		fprintf(stderr, "gx-vk: a shader module failed\n");
	return m;
}

// ---- pipelines: key = BP 0x41 blend (bits 0-15), 0x40 ZMODE (16-20), cull (21-22), EFB alpha (23) ----
// dual: the source alpha the factors use is the shader's second output (dual-source blending: the
// first output's alpha is the destination alpha constant written to the EFB)
VkBlendFactor blendFactor(int f, bool forDst, bool alpha, bool efbAlpha, bool dual = false)
{
	switch (f)
	{
	case 0: return VK_BLEND_FACTOR_ZERO;
	case 1: return VK_BLEND_FACTOR_ONE;
	case 2:
		if (forDst)
			return alpha ? (dual ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA) : VK_BLEND_FACTOR_SRC_COLOR;
		return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
	case 3:
		if (forDst)
			return alpha ? (dual ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case 4: return dual ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
	case 5: return dual ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case 6: return efbAlpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_ONE;
	default: return efbAlpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ZERO;
	}
}

int s_frontFace = -1; // SSA_VK_FRONT=cw|ccw

VkPipeline mainPipeline(uint32_t key)
{
	auto it = s_pipelines.find(key);
	if (it != s_pipelines.end())
		return it->second;
	if (s_frontFace < 0)
	{
		const char* e = getenv("SSA_VK_FRONT");
		// clockwise: GX's front faces (as Dolphin). Counter-clockwise (the old default) culled the
		// wrong side: the first level lost islands and houses, and its full-screen redraws of the
		// scene (GENMODE cull 2) were dropped every other frame, which flickered.
		s_frontFace = e && !strcmp(e, "ccw") ? 1 : 0;
	}
	const uint32_t blend = key & 0xFFFF, zmode = (key >> 16) & 31, cull = (key >> 21) & 3;
	const bool efbAlpha = (key >> 23) & 1, dual = (key >> 24) & 1;
	VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = s_tevVert;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = s_tevFrag;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	static const VkCullModeFlags culls[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_FRONT_AND_BACK};
	rs.cullMode = culls[cull];
	rs.frontFace = s_frontFace ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
	rs.depthClampEnable = VK_TRUE; // (depth clipping: tev.vert's clip distances, in GX's clip space)
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = zmode & 1;
	ds.depthWriteEnable = (zmode >> 4) & 1;
	ds.depthCompareOp = (VkCompareOp)((zmode >> 1) & 7);
	if (!ds.depthTestEnable && ds.depthWriteEnable)
	{
		// (GX writes depth with the test off; Vulkan needs the test on to write)
		ds.depthTestEnable = VK_TRUE;
		ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	}
	VkPipelineColorBlendAttachmentState att{};
	const bool blendOn = blend & 1, logicOn = (blend >> 1) & 1, sub = (blend >> 11) & 1;
	const int dstF = (blend >> 5) & 7, srcF = (blend >> 8) & 7;
	if (blend & 8)
		att.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
	if ((blend & 16) && efbAlpha)
		att.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	if (blendOn)
	{
		att.blendEnable = VK_TRUE;
		if (sub)
		{
			att.srcColorBlendFactor = att.dstColorBlendFactor = att.srcAlphaBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
		}
		else
		{
			att.srcColorBlendFactor = blendFactor(srcF, false, false, efbAlpha, dual);
			att.dstColorBlendFactor = blendFactor(dstF, true, false, efbAlpha, dual);
			att.srcAlphaBlendFactor = blendFactor(srcF, false, true, efbAlpha, dual);
			att.dstAlphaBlendFactor = blendFactor(dstF, true, true, efbAlpha, dual);
			att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
		}
		// (the destination alpha constant: the EFB's alpha becomes the first output's - that constant -
		// and isn't blended, as Dolphin's blending state does it)
		if (dual)
		{
			att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			att.alphaBlendOp = VK_BLEND_OP_ADD;
		}
	}
	else if (logicOn && s_logicOp)
	{
		cb.logicOpEnable = VK_TRUE;
		cb.logicOp = (VkLogicOp)((blend >> 12) & 15);
	}
	cb.attachmentCount = 1;
	cb.pAttachments = &att;
	const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &kColorFmt;
	ri.depthAttachmentFormat = kDepthFmt;
	VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pci.pNext = &ri;
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vin;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dy;
	pci.layout = s_mainPL;
	VkPipeline p = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(s_dev, s_pipelineCache, 1, &pci, nullptr, &p) != VK_SUCCESS)
		fprintf(stderr, "gx-vk: pipeline %06x failed\n", key);
	s_pipelines[key] = p;
	s_stats.pipelines++;
	return p;
}

bool makeCopyPipeline()
{
	VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = s_copyVert;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = s_copyFrag;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState att{};
	att.colorWriteMask = 15;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &att;
	const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &kColorFmt;
	VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pci.pNext = &ri;
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vin;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dy;
	pci.layout = s_copyPL;
	VKCHECK(vkCreateGraphicsPipelines(s_dev, s_pipelineCache, 1, &pci, nullptr, &s_copyPipeline));
	return true;
}

// an EFB clear's pipeline (copy.vert + copy.frag's clear mode, drawn into the EFB): color write mask,
// depth written or not (always passing)
std::unordered_map<uint32_t, VkPipeline> s_clearPipelines;
VkPipeline clearPipeline(uint32_t colorMask, bool zWrite)
{
	const uint32_t key = colorMask | (zWrite ? 16u : 0u);
	auto it = s_clearPipelines.find(key);
	if (it != s_clearPipelines.end())
		return it->second;
	VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = s_copyVert;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = s_copyFrag;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = zWrite ? VK_TRUE : VK_FALSE;
	ds.depthWriteEnable = zWrite ? VK_TRUE : VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	VkPipelineColorBlendAttachmentState att{};
	att.colorWriteMask = colorMask;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &att;
	const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &kColorFmt;
	ri.depthAttachmentFormat = kDepthFmt;
	VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pci.pNext = &ri;
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vin;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dy;
	pci.layout = s_copyPL;
	VkPipeline p = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(s_dev, s_pipelineCache, 1, &pci, nullptr, &p) != VK_SUCCESS)
		p = VK_NULL_HANDLE;
	s_clearPipelines[key] = p;
	return p;
}

// ---- presenting ----
bool makeSwapchain()
{
	VkHeadlessSurfaceCreateInfoEXT hci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
	VKCHECK(vkCreateHeadlessSurfaceEXT(s_instance, &hci, nullptr, &s_surface));
	VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sci.surface = s_surface;
	sci.minImageCount = 3;
	sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
	sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	sci.imageExtent = {kScreenW, kScreenH};
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sci.clipped = VK_TRUE;
	VKCHECK(vkCreateSwapchainKHR(s_dev, &sci, nullptr, &s_swapchain));
	s_swapCount = 8;
	VKCHECK(vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_swapCount, s_swapImages));
	// (views: the quick menu is drawn into the swapchain image)
	for (uint32_t i = 0; i < s_swapCount; i++)
	{
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = s_swapImages[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_B8G8R8A8_UNORM;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VKCHECK(vkCreateImageView(s_dev, &vi, nullptr, &s_swapViews[i]));
	}
	VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VKCHECK(vkCreateSemaphore(s_dev, &semi, nullptr, &s_acquired));
	fprintf(stderr, "gx-vk: presenting at %ux%u (%u swapchain images)\n", kScreenW, kScreenH, s_swapCount);
	return true;
}

void swapBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = src;
	b.dstAccessMask = dst;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---- command buffers ----
// the current frame's halves of the rings, from their starts
void resetRings()
{
	const VkDeviceSize vertHalf = s_verts.size / 4 / kInFlight, idxHalf = s_idx.size / 4 / kInFlight;
	const VkDeviceSize stateHalf = s_states.size / sizeof(GpuState) / kInFlight, xfHalf = s_xfs.size / (gxvk::kXfWords * 4) / kInFlight;
	const VkDeviceSize stagingHalf = s_staging.size / kInFlight;
	s_vertWords = vertHalf * s_set;
	s_vertEnd = s_vertWords + vertHalf;
	s_idxCount = idxHalf * s_set;
	s_idxEnd = s_idxCount + idxHalf;
	s_stateCount = stateHalf * s_set;
	s_stateEnd = s_stateCount + stateHalf;
	s_xfCount = xfHalf * s_set;
	s_xfEnd = s_xfCount + xfHalf;
	s_stagingUsed = stagingHalf * s_set;
	s_stagingEnd = s_stagingUsed + stagingHalf;
	s_xfCurrent = ~0u;
}

bool beginCommands()
{
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	s_uploadCb = s_uploadCbs[s_set];
	s_cb = s_cbs[s_set];
	VKCHECK(vkResetCommandPool(s_dev, s_pools[s_set], 0));
	VKCHECK(vkBeginCommandBuffer(s_uploadCb, &bi));
	VKCHECK(vkBeginCommandBuffer(s_cb, &bi));
	vkCmdBindIndexBuffer(s_cb, s_idx.buf, 0, VK_INDEX_TYPE_UINT32);
	s_recording = true;
	s_uploads = false;
	s_boundPipeline = VK_NULL_HANDLE;
	s_lastStateIndex = ~0u;
	return true;
}

void beginRendering()
{
	if (s_inRendering)
		return;
	transition(s_cb, s_efbColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	transition(s_cb, s_efbDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
	VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	color.imageView = s_efbColor.view;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	depth.imageView = s_efbDepth.view;
	depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {(uint32_t)(kEfbW * s_scale), (uint32_t)(kEfbH * s_scale)}};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &color;
	ri.pDepthAttachment = &depth;
	vkCmdBeginRendering(s_cb, &ri);
	// depth: GX's 24-bit depth d is d / 2^24 here (clears too). GX clamps a fragment's depth to 0xFFFFFF,
	// so the max depth is 0xFFFFFF / 2^24 (with depth clamp on): skies drawn at the far limit (SSPE52's
	// first level: viewport far 2^24, z scale 1) land on the cleared depth and pass LEQUAL. At 1.0 they
	// failed it, and Vulkan's own z clip cut their triangles at random: black, jagged sky.
	const VkViewport vp{0, 0, (float)(kEfbW * s_scale), (float)(kEfbH * s_scale), 0.0f, 16777215.0f / 16777216.0f};
	vkCmdSetViewport(s_cb, 0, 1, &vp);
	vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mainPL, 0, 1, &s_mainSet, 0, nullptr);
	s_boundPipeline = VK_NULL_HANDLE;
	// (the pending draw keeps its scissor: flushDraw begins rendering for it, and zeroing it here
	// dropped the first draw after every frame start and EFB copy, the menu's sky every other frame)
	s_inRendering = true;
}

void flushDraw()
{
	if (!s_pending.count)
		return;
	if (s_pending.pipeline)
	{
		beginRendering();
		if (s_boundPipeline != s_pending.pipeline)
		{
			vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pending.pipeline);
			s_boundPipeline = s_pending.pipeline;
		}
		vkCmdSetScissor(s_cb, 0, 1, &s_pending.scissor);
		vkCmdPushConstants(s_cb, s_mainPL, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &s_pending.pc);
		vkCmdDrawIndexed(s_cb, s_pending.count, 1, s_pending.firstIndex, 0, 0);
		s_stats.gpuDraws++;
	}
	s_pending.count = 0;
}

void endRendering()
{
	flushDraw();
	if (s_inRendering)
	{
		vkCmdEndRendering(s_cb);
		s_inRendering = false;
	}
}

void collectGarbage(int set)
{
	for (Texture& t : s_garbage[set])
	{
		if (t.slot >= 0)
			s_freeSlots.push_back(t.slot);
		destroyImage(t.im);
	}
	s_garbage[set].clear();
}

// a frame in flight has run: its fence waited for, its retired textures destroyed
bool waitSet(int set)
{
	if (s_fencePending[set])
	{
		const uint64_t waitStart = __builtin_ia32_rdtsc();
		VKCHECK(vkWaitForFences(s_dev, 1, &s_fences[set], VK_TRUE, ~0ull));
		g_ssaTicksGpuWait = g_ssaTicksGpuWait + (__builtin_ia32_rdtsc() - waitStart);
		s_fencePending[set] = false;
	}
	collectGarbage(set);
	return true;
}

// the current frame's commands to the GPU (after the vertex fetch workers are done with its rings)
bool submitCurrent()
{
	gxFetchFinish();
	endRendering();
	VKCHECK(vkEndCommandBuffer(s_uploadCb));
	VKCHECK(vkEndCommandBuffer(s_cb));
	s_recording = false;
	VkCommandBuffer cbs[2] = {s_uploadCb, s_cb};
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 2;
	si.pCommandBuffers = cbs;
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	if (s_waitAcquired)
	{
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &s_acquired;
		si.pWaitDstStageMask = &waitStage;
		s_waitAcquired = false;
	}
	VKCHECK(vkResetFences(s_dev, 1, &s_fences[s_set]));
	VKCHECK(vkQueueSubmit(s_queue, 1, &si, s_fences[s_set]));
	s_fencePending[s_set] = true;
	return true;
}

// in the middle of a frame (a ring or the staging buffer full, a readback): submit, wait for all of it
// to run, and go on recording into the same frame (resetRings false: in the middle of a draw whose
// vertices and indices are already in the rings)
bool submitAndWait(bool resetRingsToo = true)
{
	if (!submitCurrent())
		return false;
	for (int s = 0; s < kInFlight; s++)
		waitSet(s);
	const VkDeviceSize vertWords = s_vertWords, idxCount = s_idxCount, stateCount = s_stateCount, xfCount = s_xfCount;
	const uint32_t xfCurrent = s_xfCurrent;
	resetRings();
	if (!resetRingsToo)
	{
		s_vertWords = vertWords;
		s_idxCount = idxCount;
		s_stateCount = stateCount;
		s_xfCount = xfCount;
		s_xfCurrent = xfCurrent;
	}
	return beginCommands();
}

// the end of a frame: submit it, then the next frame takes the other half of everything once the
// frame before last (which used it) has run (capture: wait for this one too, for its readback)
bool submitFrame(bool waitForIt)
{
	if (!submitCurrent())
		return false;
	if (s_swapIndex != ~0u)
	{
		VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.swapchainCount = 1;
		pi.pSwapchains = &s_swapchain;
		pi.pImageIndices = &s_swapIndex;
		const VkResult r = vkQueuePresentKHR(s_queue, &pi);
		static int s_presentErrors = 0;
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && s_presentErrors++ < 5)
			fprintf(stderr, "gx-vk: vkQueuePresentKHR: %d\n", (int)r);
		s_swapIndex = ~0u;
	}
	if (waitForIt)
		waitSet(s_set);
	s_set = (s_set + 1) % kInFlight;
	waitSet(s_set);
	resetRings();
	return beginCommands();
}

// ---- the quick menu and the frame counter over the presented frame (overlay.cpp's ImGui drawing) ----
VkDescriptorSetLayout s_ovLayout = VK_NULL_HANDLE;
VkPipelineLayout s_ovPL = VK_NULL_HANDLE;
VkPipeline s_ovPipeline = VK_NULL_HANDLE;
VkDescriptorPool s_ovPool = VK_NULL_HANDLE;
VkDescriptorSet s_ovSet = VK_NULL_HANDLE;
VkSampler s_ovSampler = VK_NULL_HANDLE;
VkShaderModule s_ovVert = VK_NULL_HANDLE, s_ovFrag = VK_NULL_HANDLE;
Image s_ovAtlas;
Buffer s_ovVerts[kInFlight + 1], s_ovIdx[kInFlight + 1]; // (one set per frame in flight, one for presents while paused)
bool s_ovReady = false;
constexpr VkDeviceSize kOvVertBytes = 2u << 20, kOvIdxBytes = 1u << 20;
// presents while the game is paused (the menu over the last frame): their own command buffer
VkCommandPool s_idlePool = VK_NULL_HANDLE;
VkCommandBuffer s_idleCb = VK_NULL_HANDLE;
VkFence s_idleFence = VK_NULL_HANDLE;
VkSemaphore s_idleAcquired = VK_NULL_HANDLE;
bool s_idlePending = false;
int s_lastW = 0, s_lastH = 0; // the XFB's size at the last present
Image s_lastFrame; // (the last XFB shown: the XFB image also gets the copies VI never shows)
// the PC's screen (SSA_MENU_SHOTS=dir: the picture as it would be presented, menu included, as PNGs
// every SSA_MENU_SHOT_EVERY presents while the overlay draws something)
Image s_screen;
Buffer s_screenReadback;
bool s_shotPending = false;
bool s_smooth = false; // the texture filtering setting (Smooth: every texture bilinear), read each frame

bool initOverlay()
{
	const char* fonts = getenv("SSA_FONTS");
	unsigned char* pixels = nullptr;
	int aw = 0, ah = 0;
	if (!overlayInit(fonts ? fonts : "fonts", &pixels, &aw, &ah) || !pixels)
		return false;
	if (!makeImage(s_ovAtlas, aw, ah, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
		!makeImage(s_lastFrame, kXfbW * s_scale, kXfbH * s_scale, kColorFmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
		return false;
	const VkDeviceSize bytes = (VkDeviceSize)aw * ah * 4;
	if (s_stagingUsed + bytes > s_stagingEnd)
		return false;
	memcpy(s_staging.map + s_stagingUsed, pixels, bytes);
	transition(s_uploadCb, s_ovAtlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy c{};
	c.bufferOffset = s_stagingUsed;
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.imageExtent = {(uint32_t)aw, (uint32_t)ah, 1};
	vkCmdCopyBufferToImage(s_uploadCb, s_staging.buf, s_ovAtlas.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	transition(s_uploadCb, s_ovAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	s_stagingUsed = (s_stagingUsed + bytes + 255) & ~(VkDeviceSize)255;
	for (int i = 0; i <= kInFlight; i++)
		if (!makeBuffer(s_ovVerts[i], kOvVertBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || !makeBuffer(s_ovIdx[i], kOvIdxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
			return false;
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.minFilter = sci.magFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VKCHECK(vkCreateSampler(s_dev, &sci, nullptr, &s_ovSampler));
	VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 1;
	lci.pBindings = &b;
	VKCHECK(vkCreateDescriptorSetLayout(s_dev, &lci, nullptr, &s_ovLayout));
	const VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &s_ovLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VKCHECK(vkCreatePipelineLayout(s_dev, &plci, nullptr, &s_ovPL));
	const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &size;
	VKCHECK(vkCreateDescriptorPool(s_dev, &dpci, nullptr, &s_ovPool));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = s_ovPool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &s_ovLayout;
	VKCHECK(vkAllocateDescriptorSets(s_dev, &dai, &s_ovSet));
	VkDescriptorImageInfo ii{s_ovSampler, s_ovAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = s_ovSet;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets(s_dev, 1, &w, 0, nullptr);
	s_ovVert = shader(spv_overlay_vert, sizeof(spv_overlay_vert));
	s_ovFrag = shader(spv_overlay_frag, sizeof(spv_overlay_frag));
	if (!s_ovVert || !s_ovFrag)
		return false;
	VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = s_ovVert;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = s_ovFrag;
	stages[1].pName = "main";
	const VkVertexInputBindingDescription vb{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
	const VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(ImDrawVert, pos)},
		{1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(ImDrawVert, uv)}, {2, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(ImDrawVert, col)}};
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vin.vertexBindingDescriptionCount = 1;
	vin.pVertexBindingDescriptions = &vb;
	vin.vertexAttributeDescriptionCount = 3;
	vin.pVertexAttributeDescriptions = va;
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState att{};
	att.blendEnable = VK_TRUE;
	att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	att.colorBlendOp = VK_BLEND_OP_ADD;
	att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	att.alphaBlendOp = VK_BLEND_OP_ADD;
	att.colorWriteMask = 15;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &att;
	const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &kColorFmt;
	VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pci.pNext = &ri;
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vin;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dy;
	pci.layout = s_ovPL;
	VKCHECK(vkCreateGraphicsPipelines(s_dev, s_pipelineCache, 1, &pci, nullptr, &s_ovPipeline));
	if (s_present)
	{
		VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		cpci.queueFamilyIndex = s_queueFamily;
		VKCHECK(vkCreateCommandPool(s_dev, &cpci, nullptr, &s_idlePool));
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = s_idlePool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VKCHECK(vkAllocateCommandBuffers(s_dev, &cai, &s_idleCb));
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VKCHECK(vkCreateFence(s_dev, &fci, nullptr, &s_idleFence));
		VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VKCHECK(vkCreateSemaphore(s_dev, &semi, nullptr, &s_idleAcquired));
	}
	else if (getenv("SSA_MENU_SHOTS"))
	{
		if (!makeImage(s_screen, kScreenW, kScreenH, kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				VK_IMAGE_ASPECT_COLOR_BIT) ||
			!makeBuffer(s_screenReadback, (VkDeviceSize)kScreenW * kScreenH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
			return false;
	}
	s_ovReady = true;
	return true;
}

// the overlay's drawing into an image (in TRANSFER_DST layout, left in COLOR_ATTACHMENT), with the
// vertex/index buffers of set bufSet
void drawOverlay(VkCommandBuffer cb, VkImage img, VkImageView view, uint32_t w, uint32_t h, int bufSet, ImDrawData* dd)
{
	swapBarrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	VkDeviceSize vOff = 0, iOff = 0;
	for (int n = 0; n < dd->CmdListsCount; n++)
	{
		const ImDrawList* list = dd->CmdLists[n];
		const VkDeviceSize vb = (VkDeviceSize)list->VtxBuffer.Size * sizeof(ImDrawVert), ib = (VkDeviceSize)list->IdxBuffer.Size * sizeof(ImDrawIdx);
		if (vOff + vb > kOvVertBytes || iOff + ib > kOvIdxBytes)
			return; // (more than the buffers hold: not drawn this frame)
		memcpy(s_ovVerts[bufSet].map + vOff, list->VtxBuffer.Data, vb);
		memcpy(s_ovIdx[bufSet].map + iOff, list->IdxBuffer.Data, ib);
		vOff += vb;
		iOff += ib;
	}
	VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	att.imageView = view;
	att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {w, h}};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &att;
	vkCmdBeginRendering(cb, &ri);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ovPipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ovPL, 0, 1, &s_ovSet, 0, nullptr);
	const VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cb, 0, 1, &s_ovVerts[bufSet].buf, &zero);
	vkCmdBindIndexBuffer(cb, s_ovIdx[bufSet].buf, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	const VkViewport vp{0, 0, (float)w, (float)h, 0.0f, 1.0f};
	vkCmdSetViewport(cb, 0, 1, &vp);
	const float pc[4] = {2.0f / dd->DisplaySize.x, 2.0f / dd->DisplaySize.y, -1.0f - dd->DisplayPos.x * 2.0f / dd->DisplaySize.x,
		-1.0f - dd->DisplayPos.y * 2.0f / dd->DisplaySize.y};
	vkCmdPushConstants(cb, s_ovPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
	uint32_t vBase = 0, iBase = 0;
	for (int n = 0; n < dd->CmdListsCount; n++)
	{
		const ImDrawList* list = dd->CmdLists[n];
		for (int c = 0; c < list->CmdBuffer.Size; c++)
		{
			const ImDrawCmd& cmd = list->CmdBuffer[c];
			if (cmd.UserCallback)
				continue;
			const float x0 = std::max(cmd.ClipRect.x - dd->DisplayPos.x, 0.0f), y0 = std::max(cmd.ClipRect.y - dd->DisplayPos.y, 0.0f);
			const float x1 = std::min(cmd.ClipRect.z - dd->DisplayPos.x, (float)w), y1 = std::min(cmd.ClipRect.w - dd->DisplayPos.y, (float)h);
			if (x1 <= x0 || y1 <= y0)
				continue;
			const VkRect2D sc{{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdDrawIndexed(cb, cmd.ElemCount, 1, iBase + cmd.IdxOffset, (int32_t)(vBase + cmd.VtxOffset), 0);
		}
		vBase += (uint32_t)list->VtxBuffer.Size;
		iBase += (uint32_t)list->IdxBuffer.Size;
	}
	vkCmdEndRendering(cb);
}

// a frame (w x h of src) fitted into a screen image (cleared black; in TRANSFER_DST layout, left in it):
// 4:3 pillarboxed, or 16:9 (the aspect setting, or the game's own widescreen mode). No frame yet (w 0):
// black.
void blitXfb(VkCommandBuffer cb, Image& src, VkImage img, int w, int h)
{
	const VkClearColorValue black{{0, 0, 0, 1}};
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	swapBarrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	if (w <= 0 || h <= 0)
		return;
	transition(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	static const bool wideEnv = getenv("SSA_WIDE") && atoi(getenv("SSA_WIDE"));
	// (Auto: 16:9 when the game draws for a 16:9 TV - the Wii's widescreen setting - else 4:3)
	const int aspect = ssa::setting("aspect");
	const bool wide = wideEnv || aspect == 2 || (aspect == 0 && ssa::settingAtStart("widescreen") == 0);
	const uint32_t outW = wide ? kScreenW : kScreenH * 4 / 3;
	const int32_t x0 = (int32_t)(kScreenW - outW) / 2;
	VkImageBlit blit{};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[1] = {w, h, 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstOffsets[0] = {x0, 0, 0};
	blit.dstOffsets[1] = {x0 + (int32_t)outW, (int32_t)kScreenH, 1};
	vkCmdBlitImage(cb, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
}

// a screen image drawn by blitXfb (and drawOverlay: overlaid) to PRESENT_SRC or TRANSFER_SRC
void finishScreen(VkCommandBuffer cb, VkImage img, bool overlaid, VkImageLayout to)
{
	swapBarrier(cb, img, overlaid ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to,
		overlaid ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, to == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT : 0);
}

// ---- textures ----
uint64_t hashBytes(const uint8_t* p, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull)
{
	h ^= n;
	size_t i = 0;
	// four independent lanes of 8 bytes (one serial multiply chain was 7% of a PS4 profile)
	if (n >= 32)
	{
		uint64_t l[4] = {h, h ^ 0xC2B2AE3D27D4EB4Full, h ^ 0x165667B19E3779F9ull, h ^ 0x27D4EB2F165667C5ull};
		for (; i + 32 <= n; i += 32)
			for (int k = 0; k < 4; k++)
			{
				uint64_t v;
				memcpy(&v, p + i + 8 * k, 8);
				l[k] = (l[k] ^ v) * 0xFF51AFD7ED558CCDull;
				l[k] ^= l[k] >> 31;
			}
		h = l[0] ^ (l[1] * 0x9E3779B97F4A7C15ull) ^ (l[2] * 0xC2B2AE3D27D4EB4Full) ^ (l[3] * 0x165667B19E3779F9ull);
	}
	for (; i + 8 <= n; i += 8)
	{
		uint64_t v;
		memcpy(&v, p + i, 8);
		h = (h ^ v) * 0xFF51AFD7ED558CCDull;
		h ^= h >> 31;
	}
	for (; i < n; i++)
		h = (h ^ p[i]) * 0x100000001B3ull;
	return h;
}

uint64_t ramHash(uint32_t address, uint32_t bytes)
{
	uint32_t avail = 0;
	const uint8_t* p = gxReadRam(address, bytes, &avail);
	if (!p)
		return 0;
	return hashBytes(p, std::min(bytes, avail));
}

int allocSlot()
{
	if (s_freeSlots.empty())
		return -1;
	const int s = s_freeSlots.back();
	s_freeSlots.pop_back();
	return s;
}

void writeSlot(int slot, VkImageView view)
{
	VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = s_mainSet;
	w.dstBinding = 2;
	w.dstArrayElement = (uint32_t)slot;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets(s_dev, 1, &w, 0, nullptr);
}

void retire(Texture& t)
{
	s_garbage[s_set].push_back(t);
	t = {};
}

// a decoded texture into a new image (through the staging ring and the upload commands)
bool uploadTexture(Texture& t, int w, int h, const std::vector<uint32_t>& argb)
{
	const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
	if (bytes > s_staging.size / kInFlight)
		return false;
	if (s_stagingUsed + bytes > s_stagingEnd)
	{
		s_stats.flushes++;
		if (!submitAndWait(false))
			return false;
	}
	if (!makeImage(t.im, w, h, kColorFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
		return false;
	t.slot = allocSlot();
	if (t.slot < 0)
	{
		destroyImage(t.im);
		return false;
	}
	memcpy(s_staging.map + s_stagingUsed, argb.data(), bytes);
	transition(s_uploadCb, t.im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy c{};
	c.bufferOffset = s_stagingUsed;
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
	vkCmdCopyBufferToImage(s_uploadCb, s_staging.buf, t.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	transition(s_uploadCb, t.im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	s_stagingUsed = (s_stagingUsed + bytes + 255) & ~(VkDeviceSize)255;
	s_uploads = true;
	writeSlot(t.slot, t.im.view);
	s_stats.textures++;
	return true;
}

// MODE0: wrap s bits 0-1, t 2-3 (clamp, repeat, mirror), magnification bit 4, minification 5-7
uint32_t samplerIndex(uint32_t mode0)
{
	const uint32_t ws = std::min(mode0 & 3, 2u), wt = std::min((mode0 >> 2) & 3, 2u);
	const uint32_t mag = (mode0 >> 4) & 1, min = ((mode0 >> 5) & 7) >= 4;
	return (((ws * 3 + wt) * 2 + mag) * 2 + min) | (s_smooth ? 3u : 0u);
}

// the descriptor slot and sampler of map n's texture (0xFFFFFFFF: none)
uint32_t textureFor(int map, float size[2])
{
	GxTextureInfo ti;
	if (!gxTextureInfo(map, ti))
		return ~0u;
	size[0] = 1.0f / ti.w;
	size[1] = 1.0f / ti.h;
	const uint32_t sampler = samplerIndex(ti.mode0) << 16;
	// an EFB copy made to this address, while the RAM there is as it was
	auto e = s_efbCopies.find(ti.address);
	// (SSA_VK_COPYLOG=n: the first n lookups of addresses with an EFB copy, and what they found)
	static const int copyLog = getenv("SSA_VK_COPYLOG") ? atoi(getenv("SSA_VK_COPYLOG")) : 0;
	static int copyLogged = 0;
	if (e != s_efbCopies.end() && copyLogged < copyLog)
	{
		copyLogged++;
		fprintf(stderr, "gx-vk: frame %llu: texture %08x %dx%d fmt %d: EFB copy %dx%d made in frame %llu, RAM %s\n", (unsigned long long)s_frame,
			ti.address, ti.w, ti.h, ti.fmt, e->second.im.w, e->second.im.h, (unsigned long long)e->second.validFrame,
			ramHash(ti.address, e->second.ramBytes) == e->second.hash ? "unchanged" : "changed");
	}
	if (e != s_efbCopies.end() && e->second.im.w == ti.w && e->second.im.h == ti.h)
	{
		Texture& t = e->second;
		if (t.validFrame != s_frame)
		{
			if (ramHash(ti.address, t.ramBytes) != t.hash)
			{
				retire(t);
				s_efbCopies.erase(e);
				goto ram;
			}
			t.validFrame = s_frame;
		}
		t.lastUsed = s_frame;
		s_stats.efbCopyHits++;
		return (uint32_t)t.slot | sampler;
	}
ram:
	{
		const TexKey key{ti.address, ti.image0, ti.tlutBytes ? ti.tlut : 0u};
		Texture& t = s_textures[key];
		if (t.slot >= 0 && t.validFrame == s_frame)
		{
			t.lastUsed = s_frame;
			return (uint32_t)t.slot | sampler;
		}
		uint64_t h = ramHash(ti.address, ti.bytes);
		if (ti.tlutBytes)
			h = hashBytes(gxTmem() + (ti.tlutOffset & 0xFFFFF), std::min<uint32_t>(ti.tlutBytes, 0x100000 - (ti.tlutOffset & 0xFFFFF)), h);
		if (t.slot >= 0 && t.hash == h)
		{
			t.validFrame = t.lastUsed = s_frame;
			return (uint32_t)t.slot | sampler;
		}
		if (t.slot >= 0)
			retire(t);
		static std::vector<uint32_t> argb;
		if (!gxDecodeTexture(map, argb) || !uploadTexture(t, ti.w, ti.h, argb))
		{
			s_textures.erase(key);
			return ~0u;
		}
		// SSA_VK_DUMPTEX=address: that texture as decoded, to texdump-<address>-<frame>.png (diagnosis)
		static const uint32_t dumpAt = getenv("SSA_VK_DUMPTEX") ? (uint32_t)strtoul(getenv("SSA_VK_DUMPTEX"), nullptr, 16) : 0;
		if (dumpAt && ti.address == dumpAt)
		{
			char path[64];
			snprintf(path, sizeof(path), "texdump-%08x-%llu.png", dumpAt, (unsigned long long)s_frame);
			gxWritePng(path, ti.w, ti.h, argb.data());
			// (and the bytes in guest RAM)
			snprintf(path, sizeof(path), "texdump-%08x-%llu.bin", dumpAt, (unsigned long long)s_frame);
			uint32_t avail = 0;
			const uint8_t* raw = gxReadRam(ti.address, ti.bytes, &avail);
			if (FILE* f = raw ? fopen(path, "wb") : nullptr)
			{
				fwrite(raw, 1, std::min(ti.bytes, avail), f);
				fclose(f);
			}
		}
		t.hash = h;
		t.validFrame = t.lastUsed = s_frame;
		// SSA_VK_TEXLOG=n: the first n textures decoded from RAM with a new size/format/address
		// (frame, map, size, format, address, wrap): which textures a scene is made of
		static const int texLog = getenv("SSA_VK_TEXLOG") ? atoi(getenv("SSA_VK_TEXLOG")) : 0;
		static int texLogged = 0;
		if (texLogged < texLog)
		{
			static std::set<std::tuple<uint32_t, uint32_t>> seen;
			if (seen.insert({ti.address, ti.image0}).second)
			{
				texLogged++;
				fprintf(stderr, "gx-vk: texture at frame %llu: map %d %dx%d fmt %u at %08x mode0 %08x (decoded %llu times so far)\n",
					(unsigned long long)s_frame, map, ti.w, ti.h, (ti.image0 >> 20) & 15, ti.address, ti.mode0,
					(unsigned long long)s_stats.textures);
			}
		}
		return (uint32_t)t.slot | sampler;
	}
}

// textures unused for a while go (after this frame's commands)
void evict()
{
	for (auto it = s_textures.begin(); it != s_textures.end();)
		if (it->second.lastUsed + 600 < s_frame)
		{
			retire(it->second);
			it = s_textures.erase(it);
		}
		else
			++it;
	for (auto it = s_efbCopies.begin(); it != s_efbCopies.end();)
		if (it->second.lastUsed + 600 < s_frame)
		{
			retire(it->second);
			it = s_efbCopies.erase(it);
		}
		else
			++it;
}

// GX fog parameters (BP 0xEE A, 0xF1 C): 11-bit mantissa, 8-bit exponent, sign
float fogFloat(uint32_t v)
{
	const uint32_t bits = ((v >> 19) & 1) << 31 | ((v >> 11) & 0xFF) << 23 | (v & 0x7FF) << 12;
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

// the draw's state from BP (and the TEV registers); its index in the state buffer
uint32_t buildState()
{
	const uint32_t* bp = gxBp();
	GpuState s{};
	s.genmode = bp[0x00];
	s.alphaTest = bp[0xF3];
	// (the destination alpha constant replaces the written alpha, not the one blending uses: with
	// dual-source blending on every draw (the blend reads the shader's second output), without it only
	// on draws that don't blend)
	s.flags = ((bp[0x42] >> 8 & 1) && (s_dualSrc || !(bp[0x41] & 1)) ? 1u : 0u) | (bp[0x42] & 0xFF) << 8;
	static const uint32_t debug = getenv("SSA_VK_DEBUG") ? (uint32_t)atoi(getenv("SSA_VK_DEBUG")) : 0;
	s.pad0 = debug;
	for (int i = 0; i < 4; i++)
		s.tref[i] = bp[0x28 + i];
	for (int i = 0; i < 16; i++)
	{
		s.cenv[i] = bp[0xC0 + 2 * i];
		s.aenv[i] = bp[0xC1 + 2 * i];
	}
	for (int i = 0; i < 8; i++)
		s.ksel[i] = bp[0xF6 + i];
	memcpy(s.reg, gxTevColor(), sizeof(s.reg));
	memcpy(s.konst, gxTevKonst(), sizeof(s.konst));
	for (int i = 0; i < 8; i++)
	{
		s.tex[i] = ~0u;
		s.suSize[i][0] = (float)((bp[0x30 + 2 * i] & 0xFFFF) + 1);
		s.suSize[i][1] = (float)((bp[0x31 + 2 * i] & 0xFFFF) + 1);
	}
	// the maps the enabled stages use
	const uint32_t stages = ((bp[0x00] >> 10) & 15) + 1;
	uint32_t used = 0;
	for (uint32_t n = 0; n < stages; n++)
	{
		const uint32_t tref = bp[0x28 + (n >> 1)] >> ((n & 1) * 12);
		if (tref & 0x40)
			used |= 1u << (tref & 7);
	}
	// (and the indirect stages' maps: genmode bits 16-18 count them, BP 0x27 names them)
	for (uint32_t i = 0, ind = (bp[0x00] >> 16) & 7; i < ind && i < 4; i++)
		used |= 1u << ((bp[0x27] >> (6 * i)) & 7);
	const uint64_t texT0 = __builtin_ia32_rdtsc();
	for (int m = 0; m < 8; m++)
		if (used & (1u << m))
		{
			float size[2] = {1, 1};
			s.tex[m] = textureFor(m, size);
			s.suSize[m][2] = size[0];
			s.suSize[m][3] = size[1];
		}
	g_gxTicksTextures += __builtin_ia32_rdtsc() - texT0;
	s.fog[0] = fogFloat(bp[0xEE]);
	s.fog[1] = fogFloat(bp[0xF1]);
	s.fog[2] = (float)(bp[0xEF] & 0xFFFFFF);
	s.fog[3] = (float)(bp[0xF0] & 31);
	s.fogSel = ((bp[0xF1] >> 21) & 7) | ((bp[0xF1] >> 20) & 1) << 3;
	s.fogColor = bp[0xF2];
	for (int n = 0; n < 16; n++)
		s.indCmd[n] = bp[0x10 + n] & 0xFFFFFF;
	s.iref = bp[0x27] & 0xFFFFFF;
	s.ss0 = bp[0x25] & 0xFFFFFF;
	s.ss1 = bp[0x26] & 0xFFFFFF;
	for (int m = 0; m < 3; m++)
	{
		auto sext11 = [](uint32_t v) { return (int32_t)(v << 21) >> 21; };
		const uint32_t c0 = bp[0x06 + 3 * m], c1 = bp[0x07 + 3 * m], c2 = bp[0x08 + 3 * m];
		const int32_t scale = (int32_t)(((c0 >> 22) & 3) | ((c1 >> 22) & 3) << 2 | ((c2 >> 22) & 1) << 4);
		s.indMtx[2 * m][0] = sext11(c0);
		s.indMtx[2 * m][1] = sext11(c1);
		s.indMtx[2 * m][2] = sext11(c2);
		s.indMtx[2 * m][3] = 17 - scale;
		s.indMtx[2 * m + 1][0] = sext11(c0 >> 11);
		s.indMtx[2 * m + 1][1] = sext11(c1 >> 11);
		s.indMtx[2 * m + 1][2] = sext11(c2 >> 11);
		s.indMtx[2 * m + 1][3] = 17 - scale;
	}
	if (s_lastStateIndex != ~0u && !memcmp(&s, &s_lastState, sizeof(s)))
		return s_lastStateIndex;
	if (s_stateCount + 1 > s_stateEnd)
		return ~0u; // (reserve() flushes before this can happen)
	memcpy(s_states.map + s_stateCount * sizeof(GpuState), &s, sizeof(s));
	s_lastState = s;
	s_lastStateIndex = (uint32_t)s_stateCount++;
	s_stats.states++;
	return s_lastStateIndex;
}

// the scissor (BP 0x20/0x21, with the offset of BP 0x59) in EFB pixels
VkRect2D scissor()
{
	const uint32_t* bp = gxBp();
	const int xoff = (int)(bp[0x59] & 0x3FF) * 2, yoff = (int)((bp[0x59] >> 10) & 0x3FF) * 2;
	int x0 = (int)((bp[0x20] >> 12) & 0x7FF) - xoff, y0 = (int)(bp[0x20] & 0x7FF) - yoff;
	int x1 = (int)((bp[0x21] >> 12) & 0x7FF) - xoff + 1, y1 = (int)(bp[0x21] & 0x7FF) - yoff + 1;
	x0 = std::clamp(x0, 0, kEfbW);
	y0 = std::clamp(y0, 0, kEfbH);
	x1 = std::clamp(x1, x0, kEfbW);
	y1 = std::clamp(y1, y0, kEfbH);
	return {{x0 * s_scale, y0 * s_scale}, {(uint32_t)((x1 - x0) * s_scale), (uint32_t)((y1 - y0) * s_scale)}};
}

// the texture format whose bytes an EFB copy format fills (for the RAM range it stands for)
int copyTextureFormat(int realFmt, bool depth)
{
	switch (realFmt)
	{
	case 0: return 0;           // R4 / Z4: I4
	case 2: return 2;           // RA4: IA4
	case 3: return depth ? 3 : 3; // RA8 / Z16: IA8
	case 4: return 4;
	case 5: return 5;
	case 6: return 6;           // RGBA8 / Z24X8
	case 11: case 12: return 3; // RG8, GB8 / Z16R, Z16L: IA8
	default: return 1;          // R8, A8, G8, B8, Z8...: I8
	}
}
} // namespace

namespace gxvk
{
bool ok()
{
	return s_ok;
}

bool init()
{
	if (s_tried)
		return s_ok;
	s_tried = true;
#ifdef SSA_PS4
	s_gipa = ps4kit::vk_get_instance_proc_addr_fn();
	s_present = !getenv("SSA_VK_PRESENT") || atoi(getenv("SSA_VK_PRESENT"));
#else
	s_gipa = vkGetInstanceProcAddr;
	s_present = false;
#endif
#define VKFN_LOAD_GLOBAL(f) f = (PFN_##f)s_gipa(nullptr, #f);
	VK_FNS_GLOBAL(VKFN_LOAD_GLOBAL)
#undef VKFN_LOAD_GLOBAL
	if (!vkCreateInstance)
	{
		fprintf(stderr, "gx-vk: no vkCreateInstance\n");
		return false;
	}
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "ssa";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	const char* instanceExts[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
	if (s_present)
	{
		ici.enabledExtensionCount = 2;
		ici.ppEnabledExtensionNames = instanceExts;
	}
	VKCHECK(vkCreateInstance(&ici, nullptr, &s_instance));
	{
		const char* missing = nullptr;
#define VKFN_LOAD(f) \
	if (!(f = (PFN_##f)s_gipa(s_instance, #f)) && !missing) \
		missing = #f;
		VK_FNS(VKFN_LOAD)
		if (s_present)
		{
			VK_FNS_PRESENT(VKFN_LOAD)
		}
#undef VKFN_LOAD
		if (missing)
		{
			fprintf(stderr, "gx-vk: %s missing\n", missing);
			return false;
		}
	}
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(s_instance, &n, nullptr);
	if (!n)
	{
		fprintf(stderr, "gx-vk: no Vulkan device\n");
		return false;
	}
	std::vector<VkPhysicalDevice> devs(n);
	vkEnumeratePhysicalDevices(s_instance, &n, devs.data());
	s_phys = devs[0];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(s_phys, &props);
	fprintf(stderr, "gx-vk: %s, Vulkan %u.%u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
		VK_API_VERSION_PATCH(props.apiVersion));
	vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memProps);
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &qn, qf.data());
	s_queueFamily = ~0u;
	for (uint32_t i = 0; i < qn; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			s_queueFamily = i;
			break;
		}
	if (s_queueFamily == ~0u)
		return false;
	// features: descriptor indexing (the texture array), dynamic rendering, logic ops
	VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	f12.pNext = &f13;
	VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = &f12;
	vkGetPhysicalDeviceFeatures2(s_phys, &f2);
	if (!f13.dynamicRendering || !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingSampledImageUpdateAfterBind ||
		!f12.shaderSampledImageArrayNonUniformIndexing)
	{
		fprintf(stderr, "gx-vk: %s lacks dynamic rendering or descriptor indexing\n", props.deviceName);
		return false;
	}
	s_logicOp = f2.features.logicOp;
	// (dual-source blending: the destination alpha constant on blending draws; SSA_VK_DUALSRC=0: off,
	// the constant then only on draws that don't blend, as before v00.17)
	s_dualSrc = f2.features.dualSrcBlend && (!getenv("SSA_VK_DUALSRC") || atoi(getenv("SSA_VK_DUALSRC")));
	fprintf(stderr, "gx-vk: dual-source blending %s\n", s_dualSrc ? "on" : f2.features.dualSrcBlend ? "off (SSA_VK_DUALSRC=0)" : "not supported");
	if (!f2.features.depthClamp || !f2.features.shaderClipDistance)
	{
		fprintf(stderr, "gx-vk: %s lacks depth clamp or clip distances\n", props.deviceName);
		return false;
	}
	VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	e13.dynamicRendering = VK_TRUE;
	VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	e12.pNext = &e13;
	e12.runtimeDescriptorArray = VK_TRUE;
	e12.descriptorBindingPartiallyBound = VK_TRUE;
	e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	e12.descriptorBindingUpdateUnusedWhilePending = f12.descriptorBindingUpdateUnusedWhilePending;
	VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	e2.pNext = &e12;
	e2.features.logicOp = s_logicOp;
	e2.features.dualSrcBlend = s_dualSrc;
	e2.features.depthClamp = VK_TRUE; // (GX's depth clipping is done in tev.vert)
	e2.features.shaderClipDistance = VK_TRUE;
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = s_queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &e2;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	const char* deviceExts[] = {"VK_KHR_swapchain"};
	if (s_present)
	{
		dci.enabledExtensionCount = 1;
		dci.ppEnabledExtensionNames = deviceExts;
	}
	VKCHECK(vkCreateDevice(s_phys, &dci, nullptr, &s_dev));
	vkGetDeviceQueue(s_dev, s_queueFamily, 0, &s_queue);

	for (int set = 0; set < kInFlight; set++)
	{
		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.queueFamilyIndex = s_queueFamily;
		VKCHECK(vkCreateCommandPool(s_dev, &pci, nullptr, &s_pools[set]));
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = s_pools[set];
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 2;
		VkCommandBuffer cbs[2];
		VKCHECK(vkAllocateCommandBuffers(s_dev, &cai, cbs));
		s_uploadCbs[set] = cbs[0];
		s_cbs[set] = cbs[1];
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VKCHECK(vkCreateFence(s_dev, &fci, nullptr, &s_fences[set]));
	}
	if (s_present && !makeSwapchain())
		return false;
	s_scale = std::clamp(getenv("SSA_VK_SCALE") ? atoi(getenv("SSA_VK_SCALE")) : ssa::settingAtStart("resolution") + 1, 1, 3);
	fprintf(stderr, "gx-vk: internal resolution %dx (EFB %dx%d)\n", s_scale, kEfbW * s_scale, kEfbH * s_scale);

	if (!makeBuffer(s_verts, kVertBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !makeBuffer(s_idx, kIdxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
		!makeBuffer(s_states, kStateBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !makeBuffer(s_xfs, kXfBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
		!makeBuffer(s_staging, kStagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
		!makeBuffer(s_readback, (VkDeviceSize)kXfbW * kXfbH * 4 * s_scale * s_scale, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
		return false;
	if (!makeImage(s_efbColor, kEfbW * s_scale, kEfbH * s_scale, kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT) ||
		!makeImage(s_efbDepth, kEfbW * s_scale, kEfbH * s_scale, kDepthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
		!makeImage(s_xfb, kXfbW * s_scale, kXfbH * s_scale, kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT))
		return false;

	// samplers: wrap s/t (clamp, repeat, mirror) x magnification x minification
	for (uint32_t i = 0; i < 36; i++)
	{
		static const VkSamplerAddressMode wraps[3] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
		VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sci.minFilter = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		sci.magFilter = (i & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		sci.addressModeU = wraps[i / 12];
		sci.addressModeV = wraps[(i / 4) % 3];
		sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.maxLod = 0.0f;
		VKCHECK(vkCreateSampler(s_dev, &sci, nullptr, &s_samplers[i]));
	}
	for (uint32_t i = 36; i < 64; i++)
		s_samplers[i] = s_samplers[0];
	{
		VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		VKCHECK(vkCreateSampler(s_dev, &sci, nullptr, &s_pointSampler));
	}

	// descriptor layouts: main (vertices, states, the texture array, samplers); copy (EFB color, depth)
	{
		VkDescriptorSetLayoutBinding b[5]{};
		b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
		b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		b[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlots, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		b[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLER, 64, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		b[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
		// (the texture array: slots change while the other frame in flight runs; never ones it uses)
		const VkDescriptorBindingFlags flags[5] = {0, 0,
			VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT, 0, 0};
		VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
		bf.bindingCount = 5;
		bf.pBindingFlags = flags;
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.pNext = &bf;
		lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		lci.bindingCount = 5;
		lci.pBindings = b;
		VKCHECK(vkCreateDescriptorSetLayout(s_dev, &lci, nullptr, &s_mainLayout));
		VkDescriptorSetLayoutBinding c[2]{};
		c[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		c[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo cci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		cci.bindingCount = 2;
		cci.pBindings = c;
		VKCHECK(vkCreateDescriptorSetLayout(s_dev, &cci, nullptr, &s_copyLayout));
		VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1;
		plci.pSetLayouts = &s_mainLayout;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges = &pr;
		VKCHECK(vkCreatePipelineLayout(s_dev, &plci, nullptr, &s_mainPL));
		VkPushConstantRange cpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
		plci.pSetLayouts = &s_copyLayout;
		plci.pPushConstantRanges = &cpr;
		VKCHECK(vkCreatePipelineLayout(s_dev, &plci, nullptr, &s_copyPL));
		const VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlots},
			{VK_DESCRIPTOR_TYPE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
		VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
		dpci.maxSets = 2;
		dpci.poolSizeCount = 4;
		dpci.pPoolSizes = sizes;
		VKCHECK(vkCreateDescriptorPool(s_dev, &dpci, nullptr, &s_dpool));
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = s_dpool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &s_mainLayout;
		VKCHECK(vkAllocateDescriptorSets(s_dev, &dai, &s_mainSet));
		dai.pSetLayouts = &s_copyLayout;
		VKCHECK(vkAllocateDescriptorSets(s_dev, &dai, &s_copySet));
		VkDescriptorBufferInfo vb{s_verts.buf, 0, VK_WHOLE_SIZE}, sb{s_states.buf, 0, VK_WHOLE_SIZE}, xb{s_xfs.buf, 0, VK_WHOLE_SIZE};
		VkDescriptorImageInfo samp[64];
		for (int i = 0; i < 64; i++)
			samp[i] = {s_samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
		VkDescriptorImageInfo efb[2] = {{s_pointSampler, s_efbColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{s_pointSampler, s_efbDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
		VkWriteDescriptorSet w[6]{};
		for (auto& x : w)
			x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[0].dstSet = s_mainSet;
		w[0].dstBinding = 0;
		w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w[0].pBufferInfo = &vb;
		w[1] = w[0];
		w[1].dstBinding = 1;
		w[1].pBufferInfo = &sb;
		w[2].dstSet = s_mainSet;
		w[2].dstBinding = 3;
		w[2].descriptorCount = 64;
		w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		w[2].pImageInfo = samp;
		w[3].dstSet = s_copySet;
		w[3].dstBinding = 0;
		w[3].descriptorCount = 1;
		w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[3].pImageInfo = &efb[0];
		w[4] = w[3];
		w[4].dstBinding = 1;
		w[4].pImageInfo = &efb[1];
		w[5] = w[0];
		w[5].dstBinding = 4;
		w[5].pBufferInfo = &xb;
		vkUpdateDescriptorSets(s_dev, 6, w, 0, nullptr);
	}
	s_tevVert = shader(spv_tev_vert, sizeof(spv_tev_vert));
	s_tevFrag = s_dualSrc ? shader(spv_tev_frag_dual, sizeof(spv_tev_frag_dual)) : shader(spv_tev_frag, sizeof(spv_tev_frag));
	s_copyVert = shader(spv_copy_vert, sizeof(spv_copy_vert));
	s_copyFrag = shader(spv_copy_frag, sizeof(spv_copy_frag));
	if (!s_tevVert || !s_tevFrag || !s_copyVert || !s_copyFrag)
		return false;
	VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	VKCHECK(vkCreatePipelineCache(s_dev, &pcci, nullptr, &s_pipelineCache));
	if (!makeCopyPipeline())
		return false;
	for (int i = (int)kSlots - 1; i >= 0; i--)
		s_freeSlots.push_back(i);
	resetRings();
	if (!beginCommands())
		return false;
	// the EFB starts black with the far depth
	beginRendering();
	VkClearAttachment ca[2]{};
	ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ca[0].clearValue.color = {{0, 0, 0, 1}};
	ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	ca[1].clearValue.depthStencil = {1.0f, 0};
	VkClearRect cr{{{0, 0}, {(uint32_t)(kEfbW * s_scale), (uint32_t)(kEfbH * s_scale)}}, 0, 1};
	vkCmdClearAttachments(s_cb, 2, ca, 1, &cr);
	// the quick menu and the frame counter (without them the game still runs)
	if (!initOverlay())
		fprintf(stderr, "gx-vk: the quick menu could not start (no overlay)\n");
	s_smooth = ssa::setting("filter") == 1;
	s_ok = true;
	fprintf(stderr, "gx-vk: %s (Vulkan %u.%u), logic ops %s\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
		s_logicOp ? "on" : "off");
	return true;
}

void setRendering(bool on)
{
	s_render = on && s_ok;
}

bool rendering()
{
	return s_render;
}

uint32_t* reserve(uint32_t words, uint32_t indexCount, uint32_t** indices, uint32_t* firstWord)
{
	auto full = [&]() {
		return s_vertWords + words > s_vertEnd || s_idxCount + indexCount > s_idxEnd || s_stateCount + 1 > s_stateEnd || s_xfCount + 1 > s_xfEnd;
	};
	if (full())
	{
		s_stats.flushes++;
		submitAndWait();
		if (full())
			return nullptr; // (a draw larger than the rings)
	}
	*firstWord = (uint32_t)s_vertWords;
	*indices = (uint32_t*)s_idx.map + s_idxCount;
	uint32_t* out = (uint32_t*)s_verts.map + s_vertWords;
	s_vertWords += words;
	return out;
}

uint32_t* xfBlock(bool changed, uint32_t* index)
{
	if (!changed && s_xfCurrent != ~0u)
	{
		*index = s_xfCurrent;
		return nullptr;
	}
	// (reserve() made room)
	s_stats.xfBlocks++;
	s_xfCurrent = *index = (uint32_t)s_xfCount++;
	return (uint32_t*)s_xfs.map + (size_t)s_xfCurrent * kXfWords;
}

void draw(uint32_t numTex, uint32_t indexCount)
{
	s_stats.draws++;
	if (!indexCount)
		return;
	const uint32_t* bp = gxBp();
	// (bit 24: a blending draw with the destination alpha constant, blended through the second output)
	const uint32_t key = (bp[0x41] & 0xFFFF) | (bp[0x40] & 31) << 16 | ((bp[0x00] >> 14) & 3) << 21 | ((bp[0x43] & 7) == 1 ? 1u : 0u) << 23 |
		(s_dualSrc && (bp[0x42] >> 8 & 1) && (bp[0x41] & 1) ? 1u : 0u) << 24;
	// (SSA_VK_SKIP_KEY=mask=value: leave out draws whose pipeline key matches, for gx_replay)
	static uint32_t skipMask = 0, skipValue = 1;
	static bool skipParsed = false;
	if (!skipParsed)
	{
		skipParsed = true;
		if (const char* e = getenv("SSA_VK_SKIP_KEY"))
			sscanf(e, "%x=%x", &skipMask, &skipValue);
	}
	if (skipMask && (key & skipMask) == skipValue)
	{
		s_idxCount += indexCount;
		return;
	}
	const VkPipeline pipeline = mainPipeline(key);
	// the state: the last one while no BP write or EFB copy came between (within this frame and
	// recording: a flush starts the state ring again); built (may upload textures) otherwise
	static uint32_t s_stateVersion = ~0u;
	static uint64_t s_stateFrame = ~0ull;
	uint32_t state;
	if (s_lastStateIndex != ~0u && g_gxStateVersion == s_stateVersion && s_frame == s_stateFrame)
		state = s_lastStateIndex;
	else
	{
		state = buildState();
		s_stateVersion = g_gxStateVersion;
		s_stateFrame = s_frame;
	}
	const VkRect2D sc = scissor();
	const uint32_t firstIdx = (uint32_t)s_idxCount;
	s_idxCount += indexCount;
	if (state == ~0u || !pipeline)
		return;
	const PushConstants pc{state, numTex, 0, 0};
	static int trace = getenv("SSA_VK_TRACE") ? atoi(getenv("SSA_VK_TRACE")) : 0;
	if (trace > 0)
	{
		trace--;
		const uint32_t* v = (const uint32_t*)s_verts.map + (size_t)((const uint32_t*)s_idx.map)[firstIdx];
		float c[4];
		memcpy(c, v + 1, 16); // (the CPU-transform layout; with fetched vertices: the position)
		const GpuState& gs = *(const GpuState*)(s_states.map + (size_t)state * sizeof(GpuState));
		fprintf(stderr, "gx-vk: draw %llu: key %06x scissor %d,%d %ux%u, %u indices, v0 clip %.3f %.3f %.3f %.3f col %08x tex %u; genmode %06x at %06x cenv0 %06x aenv0 %06x tref0 %06x tex0 %08x prev %d %d %d %d\n",
			(unsigned long long)s_stats.draws, key, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, indexCount, c[0], c[1], c[2], c[3], v[5], numTex,
			gs.genmode, gs.alphaTest, gs.cenv[0], gs.aenv[0], gs.tref[0], gs.tex[0], gs.reg[0][0], gs.reg[0][1], gs.reg[0][2], gs.reg[0][3]);
		fprintf(stderr, "    k0 %d %d %d %d k1 %d %d %d %d ksel %06x %06x su %g %g size %g %g tc0 %g %g %g\n", gs.konst[0][0], gs.konst[0][1], gs.konst[0][2],
			gs.konst[0][3], gs.konst[1][0], gs.konst[1][1], gs.konst[1][2], gs.konst[1][3], gs.ksel[0], gs.ksel[1], gs.suSize[0][0], gs.suSize[0][1],
			gs.suSize[0][2], gs.suSize[0][3], numTex ? *(const float*)&v[7] : 0.f, numTex ? *(const float*)&v[8] : 0.f, numTex ? *(const float*)&v[9] : 0.f);
	}
	if (s_pending.count && s_pending.pipeline == pipeline && !memcmp(&s_pending.pc, &pc, sizeof(pc)) && !memcmp(&s_pending.scissor, &sc, sizeof(sc)) &&
		s_pending.firstIndex + s_pending.count == firstIdx)
	{
		s_pending.count += indexCount;
		return;
	}
	flushDraw();
	s_pending.pipeline = pipeline;
	s_pending.pc = pc;
	s_pending.scissor = sc;
	s_pending.firstIndex = firstIdx;
	s_pending.count = indexCount;
}

void recordPresent(int w, int h); // (below)

bool efbCopy(const std::string& capturePath)
{
	if (!s_ok)
		return false;
	const uint32_t* bp = gxBp();
	const uint32_t v = bp[0x52];
	const bool toXfb = (v >> 14) & 1, clear = (v >> 11) & 1;
	if (!s_render)
	{
		if (toXfb)
		{
			s_frame++;
			evict();
		}
		return false;
	}
	s_stats.efbCopies++;
	int sx = (int)(bp[0x49] & 0x3FF), sy = (int)((bp[0x49] >> 10) & 0x3FF);
	int w = (int)(bp[0x4A] & 0x3FF) + 1, h = (int)((bp[0x4A] >> 10) & 0x3FF) + 1;
	sx = std::min(sx, kEfbW - 1);
	sy = std::min(sy, kEfbH - 1);
	w = std::min(w, kEfbW - sx);
	h = std::min(h, kEfbH - sy);
	const uint32_t pixFmt = bp[0x43] & 7;
	const bool efbAlpha = pixFmt == 1, depth = pixFmt == 3 && !toXfb;
	const bool half = !toXfb && ((v >> 9) & 1);
	const uint32_t tpf = (v >> 3) & 15, realFmt = tpf / 2 + (tpf & 1) * 8;
	const uint32_t dst = (bp[0x4B] & 0xFFFFFF) << 5;
	const int dw = half ? std::max(1, w / 2) : w, dh = half ? std::max(1, h / 2) : h;

	endRendering();
	// the destination: the XFB image or the copy kept for this address
	Texture* target = nullptr;
	Image* im = &s_xfb;
	if (!toXfb)
	{
		Texture& t = s_efbCopies[dst];
		if (t.slot >= 0 && (t.im.w != dw * s_scale || t.im.h != dh * s_scale))
			retire(t);
		if (t.slot < 0)
		{
			if (!makeImage(t.im, dw * s_scale, dh * s_scale, kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
				(t.slot = allocSlot()) < 0)
			{
				destroyImage(t.im);
				s_efbCopies.erase(dst);
				return false;
			}
			writeSlot(t.slot, t.im.view);
		}
		t.ramBytes = gxTextureBytes(copyTextureFormat((int)realFmt, depth), dw, dh);
		t.hash = ramHash(dst, t.ramBytes);
		t.validFrame = t.lastUsed = s_frame;
		target = &t;
		im = &t.im;
	}
	transition(s_cb, s_efbColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	transition(s_cb, s_efbDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
	transition(s_cb, *im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	{
		VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		color.imageView = im->view;
		color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.loadOp = toXfb ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, {(uint32_t)std::min(dw * s_scale, im->w), (uint32_t)std::min(dh * s_scale, im->h)}};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &color;
		vkCmdBeginRendering(s_cb, &ri);
		const VkViewport vp{0, 0, (float)(dw * s_scale), (float)(dh * s_scale), 0.0f, 1.0f};
		vkCmdSetViewport(s_cb, 0, 1, &vp);
		vkCmdSetScissor(s_cb, 0, 1, &ri.renderArea);
		vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_copyPipeline);
		vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_copyPL, 0, 1, &s_copySet, 0, nullptr);
		struct
		{
			int32_t src[4];
			uint32_t mode, pad[3];
		} pc{{sx * s_scale, sy * s_scale, w * s_scale, h * s_scale}, realFmt | (depth ? 16u : 0u) | (((v >> 15) & 1) ? 32u : 0u) | (half ? 64u : 0u) | (efbAlpha ? 128u : 0u) | (toXfb ? 256u : 0u), {}};
		// (the copy filter's seven weights (BP 0x53/0x54, 6 bits each) summed: the copy's brightness, x/64,
		// as Dolphin does with its copy filter off - no vertical blend of the rows, only their total)
		pc.pad[1] = (bp[0x53] & 63) + ((bp[0x53] >> 6) & 63) + ((bp[0x53] >> 12) & 63) + ((bp[0x53] >> 18) & 63) + (bp[0x54] & 63) +
			((bp[0x54] >> 6) & 63) + ((bp[0x54] >> 12) & 63);
		if (!pc.pad[1])
			pc.pad[1] = 64; // (never set: as 1.0)
		vkCmdPushConstants(s_cb, s_copyPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		vkCmdDraw(s_cb, 3, 1, 0, 0);
		vkCmdEndRendering(s_cb);
		s_boundPipeline = VK_NULL_HANDLE;
	}
	transition(s_cb, *im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	// (SSA_VK_DUMP_COPIES=dir: every EFB copy to a texture as a PNG, for renderer work in gx_replay)
	static const char* dumpDir = getenv("SSA_VK_DUMP_COPIES");
	if (dumpDir && target)
	{
		static int dumped = 0;
		transition(s_cb, *im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkBufferImageCopy c{};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {(uint32_t)(dw * s_scale), (uint32_t)(dh * s_scale), 1};
		vkCmdCopyImageToBuffer(s_cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_readback.buf, 1, &c);
		transition(s_cb, *im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		submitAndWait(false);
		char path[512];
		snprintf(path, sizeof(path), "%s/copy%03d-f%llu-%08x-%dx%d-fmt%u%s.png", dumpDir, dumped++, (unsigned long long)s_frame, dst, dw, dh, realFmt,
			depth ? "-depth" : "");
		gxWritePng(path, dw * s_scale, dh * s_scale, (const uint32_t*)s_readback.map);
	}

	// (SSA_VK_EFBLOG=n: the first n copies to textures, with the clear and the PE update masks)
	static const int efbLog = getenv("SSA_VK_EFBLOG") ? atoi(getenv("SSA_VK_EFBLOG")) : 0;
	static int efbLogged = 0;
	if (!toXfb && efbLogged < efbLog)
	{
		efbLogged++;
		fprintf(stderr, "gx-vk: frame %llu: EFB copy to %08x %dx%d fmt %u from (%d,%d) %dx%d, clear %d; CMODE0 %06x ZMODE %06x ZCTRL %06x clear color %06x/%06x; "
			"copy %06x filter %06x %06x\n",
			(unsigned long long)s_frame, dst, dw, dh, realFmt, sx, sy, w, h, (int)clear, bp[0x41] & 0xFFFFFF, bp[0x40] & 0xFFFFFF, bp[0x43] & 0xFFFFFF,
			bp[0x4F] & 0xFFFFFF, bp[0x50] & 0xFFFFFF, bp[0x52] & 0xFFFFFF, bp[0x53] & 0xFFFFFF, bp[0x54] & 0xFFFFFF);
	}
	// clear the copied rectangle (BP 0x4F/0x50 color, 0x51 depth) as the update masks allow (Dolphin's
	// ClearScreen): color with CMODE0's color update, alpha with its alpha update when the pixel
	// format has alpha, depth with ZMODE's update. Only some of the color channels: a quad with that
	// write mask (SSPE52's depth-of-field pass clears the alpha it blends with and keeps the scene's
	// colors: clearing those too left gameplay black under the HUD)
	if (clear)
	{
		const bool colorOn = (bp[0x41] >> 3) & 1, alphaOn = ((bp[0x41] >> 4) & 1) && efbAlpha, zOn = (bp[0x40] >> 4) & 1;
		const uint32_t ar = bp[0x4F], gb = bp[0x50];
		const VkRect2D rect{{sx * s_scale, sy * s_scale}, {(uint32_t)(w * s_scale), (uint32_t)(h * s_scale)}};
		if (colorOn == alphaOn && (colorOn || zOn))
		{
			beginRendering();
			VkClearAttachment ca[2]{};
			uint32_t n = 0;
			if (colorOn)
			{
				ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				ca[n].clearValue.color = {{(ar & 0xFF) / 255.0f, ((gb >> 8) & 0xFF) / 255.0f, (gb & 0xFF) / 255.0f, ((ar >> 8) & 0xFF) / 255.0f}};
				n++;
			}
			if (zOn)
			{
				ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
				ca[n].clearValue.depthStencil = {(bp[0x51] & 0xFFFFFF) / 16777216.0f, 0};
				n++;
			}
			VkClearRect cr{rect, 0, 1};
			vkCmdClearAttachments(s_cb, n, ca, 1, &cr);
		}
		else if (colorOn || alphaOn)
		{
			const uint32_t mask = colorOn ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT) : VK_COLOR_COMPONENT_A_BIT;
			if (const VkPipeline p = clearPipeline(mask, zOn))
			{
				beginRendering();
				vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
				vkCmdSetScissor(s_cb, 0, 1, &rect);
				struct
				{
					int32_t src[4];
					uint32_t mode, color, depth, pad;
				} pc{{0, 0, 0, 0}, 512u, (ar & 0xFF) | (gb & 0xFF00) | (gb & 0xFF) << 16 | ((ar >> 8) & 0xFF) << 24, bp[0x51] & 0xFFFFFF, 0};
				vkCmdPushConstants(s_cb, s_copyPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
				vkCmdDraw(s_cb, 3, 1, 0, 0);
				// (the main pipelines' descriptor set again: the copy layout's set 0 differs)
				vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mainPL, 0, 1, &s_mainSet, 0, nullptr);
				s_boundPipeline = VK_NULL_HANDLE;
			}
		}
	}

	if (toXfb)
	{
		// SSA_VK_XFBLOG=n: the first n XFB copies, their RAM address against VI's top-field address
		static const int xfbLog = getenv("SSA_VK_XFBLOG") ? atoi(getenv("SSA_VK_XFBLOG")) : 0;
		static int xfbLogged = 0;
		if (xfbLogged < xfbLog)
		{
			xfbLogged++;
			fprintf(stderr, "gx-vk: XFB copy %d: to %08x, %dx%d from (%d,%d), clear %d; VI TFBL %08x TFBR %08x\n", xfbLogged, dst, dw, dh, sx, sy,
				(int)clear, ::mmioRegValue ? ::mmioRegValue(0xCC00201C) : 0, ::mmioRegValue ? ::mmioRegValue(0xCC002020) : 0);
		}
		// only copies into a buffer VI shows are presented: the game also copies frames into RAM it
		// never displays (SSPE52: twice a frame into 0x00662840, between its two XFBs in MEM2), and
		// presenting those made the screen flash. The XFBs are the addresses VI's top-field register
		// has held (bit 28: the address in 32-byte units). SSA_VK_PRESENT_ALL=1: every copy (old way).
		static const bool presentAll = getenv("SSA_VK_PRESENT_ALL") && atoi(getenv("SSA_VK_PRESENT_ALL"));
		static uint32_t s_shown[8] = {};
		static int s_shownCount = 0;
		if (::mmioRegValue)
		{
			const uint32_t tfbl = ::mmioRegValue(0xCC00201C);
			const uint32_t shown = ((tfbl >> 28) & 1) ? (tfbl & 0xFFFFFF) << 5 : tfbl & 0xFFFFFF;
			if (shown && std::find(s_shown, s_shown + s_shownCount, shown) == s_shown + s_shownCount)
			{
				s_shown[s_shownCount % 8] = shown;
				s_shownCount = std::min(s_shownCount + 1, 8);
				fprintf(stderr, "gx-vk: VI shows the XFB at %08x\n", shown);
			}
		}
		const bool displayed = presentAll || !::mmioRegValue || std::find(s_shown, s_shown + s_shownCount, dst) != s_shown + s_shownCount;
		// the frame ends: read back a captured frame (one VI shows), submit, wait
		const bool capture = !capturePath.empty() && displayed;
		if (capture)
		{
			endRendering();
			transition(s_cb, s_xfb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy c{};
			c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c.imageExtent = {(uint32_t)(dw * s_scale), (uint32_t)(std::min(dh, kXfbH) * s_scale), 1};
			vkCmdCopyImageToBuffer(s_cb, s_xfb.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_readback.buf, 1, &c);
			transition(s_cb, s_xfb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		if ((s_present || s_screen.img) && displayed)
			recordPresent(dw * s_scale, std::min(dh, kXfbH) * s_scale);
		s_stats.frames++;
		const bool shot = s_shotPending;
		submitFrame(capture || shot);
		if (capture)
			gxWritePng(capturePath, dw * s_scale, std::min(dh, kXfbH) * s_scale, (const uint32_t*)s_readback.map);
		if (shot)
		{
			static int shots = 0;
			char path[512];
			snprintf(path, sizeof(path), "%s/menu%04d.png", getenv("SSA_MENU_SHOTS"), shots++);
			gxWritePng(path, kScreenW, kScreenH, (const uint32_t*)s_screenReadback.map);
			s_shotPending = false;
		}
		s_smooth = ssa::setting("filter") == 1;
		s_frame++;
		evict();
		return capture;
	}
	return false;
}

// the frame's swapchain image: acquired, the XFB scaled in, the quick menu / frame counter over it
// (presented after the submit). The PC with SSA_MENU_SHOTS: the same into s_screen, read back.
void recordPresent(int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	endRendering();
	ImDrawData* dd = nullptr;
	if (s_ovReady)
	{
		if (!s_present)
			overlayScriptTick();
		overlayNotePresent();
		dd = overlayFrame(kScreenW, kScreenH);
		// (kept for the presents while the game is paused)
		transition(s_cb, s_lastFrame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		transition(s_cb, s_xfb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit keep{};
		keep.srcSubresource = keep.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		keep.srcOffsets[1] = keep.dstOffsets[1] = {w, h, 1};
		vkCmdBlitImage(s_cb, s_xfb.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_lastFrame.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &keep, VK_FILTER_NEAREST);
		transition(s_cb, s_lastFrame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		s_lastW = w;
		s_lastH = h;
	}
	if (!s_present)
	{
		static const int every = getenv("SSA_MENU_SHOT_EVERY") ? std::max(1, atoi(getenv("SSA_MENU_SHOT_EVERY"))) : 15;
		static int count = 0;
		static const bool scripted = getenv("SSA_MENU_SCRIPT") != nullptr;
		if (!dd || (scripted ? !overlayScriptShot() : count++ % every != 0))
			return;
		transition(s_cb, s_screen, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		blitXfb(s_cb, s_xfb, s_screen.img, w, h);
		drawOverlay(s_cb, s_screen.img, s_screen.view, kScreenW, kScreenH, s_set, dd);
		finishScreen(s_cb, s_screen.img, true, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		s_screen.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkBufferImageCopy c{};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {kScreenW, kScreenH, 1};
		vkCmdCopyImageToBuffer(s_cb, s_screen.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_screenReadback.buf, 1, &c);
		s_shotPending = true;
		return;
	}
	uint32_t index = 0;
	const VkResult r = vkAcquireNextImageKHR(s_dev, s_swapchain, ~0ull, s_acquired, VK_NULL_HANDLE, &index);
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
	{
		static int s_errors = 0;
		if (s_errors++ < 5)
			fprintf(stderr, "gx-vk: vkAcquireNextImageKHR: %d\n", (int)r);
		return;
	}
	s_waitAcquired = true;
	const VkImage img = s_swapImages[index];
	swapBarrier(s_cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	blitXfb(s_cb, s_xfb, img, w, h);
	if (dd)
		drawOverlay(s_cb, img, s_swapViews[index], kScreenW, kScreenH, s_set, dd);
	finishScreen(s_cb, img, dd != nullptr, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	s_swapIndex = index;
}

// while the game is paused (the quick menu): the last frame shown again with the menu over it, in a
// command buffer of its own (the frame being recorded stays as it is). On the thread that submits the
// frames; paced by the swapchain (FIFO). False: nothing presented (no overlay, no swapchain).
bool presentIdle()
{
	if (!s_ok || !s_ovReady || !s_present || !s_idleCb)
		return false;
	if (s_idlePending)
	{
		VKCHECK(vkWaitForFences(s_dev, 1, &s_idleFence, VK_TRUE, ~0ull));
		s_idlePending = false;
	}
	ImDrawData* dd = overlayFrame(kScreenW, kScreenH);
	uint32_t index = 0;
	const VkResult r = vkAcquireNextImageKHR(s_dev, s_swapchain, ~0ull, s_idleAcquired, VK_NULL_HANDLE, &index);
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		return false;
	VKCHECK(vkResetCommandPool(s_dev, s_idlePool, 0));
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHECK(vkBeginCommandBuffer(s_idleCb, &bi));
	const VkImage img = s_swapImages[index];
	swapBarrier(s_idleCb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	blitXfb(s_idleCb, s_lastFrame, img, s_lastW, s_lastH);
	if (dd)
		drawOverlay(s_idleCb, img, s_swapViews[index], kScreenW, kScreenH, kInFlight, dd);
	finishScreen(s_idleCb, img, dd != nullptr, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	VKCHECK(vkEndCommandBuffer(s_idleCb));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &s_idleCb;
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &s_idleAcquired;
	si.pWaitDstStageMask = &waitStage;
	VKCHECK(vkResetFences(s_dev, 1, &s_idleFence));
	VKCHECK(vkQueueSubmit(s_queue, 1, &si, s_idleFence));
	s_idlePending = true;
	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.swapchainCount = 1;
	pi.pSwapchains = &s_swapchain;
	pi.pImageIndices = &index;
	const VkResult pr = vkQueuePresentKHR(s_queue, &pi);
	return pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR;
}

void report()
{
	if (!s_tried)
		return;
	fprintf(stderr, "gx-vk: %llu frames drawn, %llu draws -> %llu GPU draws, %llu states, %llu XF blocks, %llu pipelines, %llu textures uploaded, %llu EFB copies (%llu sampled), %llu mid-frame flushes\n",
		(unsigned long long)s_stats.frames, (unsigned long long)s_stats.draws, (unsigned long long)s_stats.gpuDraws, (unsigned long long)s_stats.states,
		(unsigned long long)s_stats.xfBlocks, (unsigned long long)s_stats.pipelines, (unsigned long long)s_stats.textures, (unsigned long long)s_stats.efbCopies, (unsigned long long)s_stats.efbCopyHits,
		(unsigned long long)s_stats.flushes);
}
} // namespace gxvk
