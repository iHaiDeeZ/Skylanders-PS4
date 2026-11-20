// jittest: what the recompiler and interpreter need from the rest of Cemu, as host stand-ins
#include <sys/mman.h>
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "Common/FileStream.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "util/MemMapper/MemMapper.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/GamePatch.h"
#include "util/helpers/helpers.h"

bool ActiveSettings::DumpRecompilerFunctionsEnabled() { return false; }
CPUMode ActiveSettings::GetCPUMode() { return CPUMode::MulticoreRecompiler; }
TitleId CafeSystem::GetForegroundTitleId() { return 0; }
FileStream* FileStream::createFile2(const fs::path& path) { return nullptr; }
sint32 FileStream::writeData(const void* data, sint32 length) { return 0; }
FileStream::~FileStream() {}
std::unique_ptr<GDBServer> g_gdbstub;
void GDBServer::HandleTrapInstruction(PPCInterpreter_t* hCPU) {}
extern "C" void jittest_noop() {} // the GDB server destructor (never called: g_gdbstub stays empty), see build.sh
void debugger_enterTW(PPCInterpreter_t* hCPU, bool isSingleStep) {}
bool GamePatch_IsNonReturnFunction(uint32 hleIndex) { return false; }
void LatteBufferCache_notifyDCFlush(MPTR address, uint32 size) {}
uint32 RPLLoader_MakePPCCallable(void (*ppcCallableExport)(struct PPCInterpreter_t* hCPU)) { return 0; }
void SetThreadName(const char* name) {}
uint64 s_loggingFlagMask = ~0ull; // every log type (JT_IML prints with LogType::Force)
bool cemuLog_log(LogType type, std::string_view text)
{
	fprintf(stderr, "log: %.*s\n", (int)text.size(), text.data());
	return true;
}
uint64 _rdtscFrequency = 1600000000;
uint64 PPCTimer_getFromRDTSC() { return 0; }
bool g_crema_return_entries = true;

namespace coreinit
{
	void OSGetCodegenVirtAddrRangeInternal(uint32& rangeStart, uint32& rangeSize) { rangeStart = 0; rangeSize = 0; }
	void codeGenHandleICBI(uint32 ea) {}
	bool codeGenShouldAvoid() { return false; }
	// (the time base: 0 for the tests, comparable runs; the Skylanders runtime sets a real clock)
	uint64 (*g_ssaTimeBase)() = nullptr;
	uint64 OSGetSystemTime() { return g_ssaTimeBase ? g_ssaTimeBase() : 0; }
}

#ifndef SSA_PS4 // (the PS4 build links Crema's MemMapperPS4.cpp)
namespace MemMapper
{
	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		void* p = mmap(baseAddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		return p == MAP_FAILED ? nullptr : p;
	}
	void FreeReservation(void* baseAddr, size_t size) { munmap(baseAddr, size); }
	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		int prot = PROT_READ | PROT_WRITE;
		if (permissionFlags == PAGE_PERMISSION::P_RWX)
			prot |= PROT_EXEC;
		if (fromReservation)
		{
			if (mprotect(baseAddr, size, prot) != 0)
				return nullptr;
			return baseAddr;
		}
		void* p = mmap(baseAddr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		return p == MAP_FAILED ? nullptr : p;
	}
	void FreeMemory(void* baseAddr, size_t size, bool fromReservation) {}
}
#endif

uint8* memory_base = nullptr;
std::vector<MMURange*> g_mmuRanges;
MMURange::MMURange(const uint32 baseAddress, const uint32 size, MMU_MEM_AREA_ID areaId, const std::string_view name, MFLAG flags) : baseAddress(baseAddress), size(size), initSize(size), areaId(areaId), name(name), flags(flags) {}
MMURange mmuRange_TRAMPOLINE_AREA{0x00E00000, 0x00200000, MMU_MEM_AREA_ID::CODE_TRAMPOLINE, "TRAMPOLINE_AREA"};
MMURange mmuRange_CODECAVE{0x01800000, 0x00400000, MMU_MEM_AREA_ID::CODE_CAVE, "CODECAVE"};
uint8* memory_getPointerFromVirtualOffset(uint32 virtualOffset) { return memory_base + virtualOffset; }
// JT_MAPPED=1 (RPX mode): only the areas Cemu maps (Cafe/HW/MMU/MMU.cpp) are accessible and the
// others fault, as on the console; JT_MAPPED=2 faults the same but calls everything accessible (the
// boundary tracker of 3.32 and before, which read wherever a branch went)
int g_jtMapped = 0;
uint32 g_jtMappedAreas[4][2] = {{0x00010000, 0x00100000}, {0x00E00000, 0x01000000}, {0x01800000, 0x01C00000}, {0x02000000, 0x50000000}};
bool memory_isAddressRangeAccessible(MPTR virtualAddress, uint32 size)
{
	if (g_jtMapped != 1)
		return true;
	for (auto& area : g_jtMappedAreas)
		if (virtualAddress >= area[0] && virtualAddress < area[1])
			return size <= area[1] - virtualAddress;
	return false;
}
uint32 memory_readU32(uint32 address) { return _swapEndianU32(*(uint32*)(memory_base + address)); }

#ifndef SSA_PS4 // (the PS4 build has ps4-kit's)
extern "C" void ps4_sampler_register_thread(const char* name) {}
extern "C" void ps4_boost_thread() {}
extern "C" void ps4_boot_trace(const char* stage) { fprintf(stderr, "trace: %s\n", stage); }
extern "C" void ps4_trace_callers(const char* what) { fprintf(stderr, "assert: %s\n", what); }
extern "C" size_t ps4rt_heap_footprint(void) { return 0; }
extern "C" size_t ps4rt_heap_size(void) { return 0; }
#endif

#ifdef SSA_PS4
// (the PC test driver's: driver.cpp defines the hook, build.sh points the GDB server's destructor at
// jittest_noop; neither is used in the app)
struct ppcImlGenContext_t;
bool (*g_jtAotHook)(ppcImlGenContext_t& ctx) = nullptr;
#endif
