// Guest memory on the host (host_ctx.h): Linux (the PC test host) and the PS4.
//
// Linux: a 4 GiB PROT_NONE reservation, mirrors through one memfd mapped MAP_SHARED at each view.
// PS4: the reservation from sceKernelReserveVirtualRange (large PROT_NONE mmaps fail there), each
// block one run of direct memory mapped at every view (direct memory may be mapped more than once),
// zeroed by hand (direct memory isn't). Pages are 16 KiB on the PS4.
#include "host_ctx.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef SSA_PS4
#include <sys/types.h>
extern "C"
{
	int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
	int32_t sceKernelMunmap(void* addr, size_t len);
	int32_t sceKernelMprotect(const void* addr, size_t len, int32_t prot);
	size_t sceKernelGetDirectMemorySize(void);
	int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* physOut);
	int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align);
	int32_t sceKernelEnableDmemAliasing();
	int32_t sceKernelEnableDmemAliasing2();
}

namespace
{
constexpr int32_t kMapFixed = 0x10;
constexpr int32_t kProtRead = 0x1, kProtWrite = 0x2;
constexpr int32_t kWbOnion = 0;

size_t roundPage(size_t n)
{
	return (n + hostctx::kPage - 1) & ~(hostctx::kPage - 1);
}

bool allocDirect(size_t size, off_t* phys)
{
	const size_t align = size >= 0x200000 ? 0x200000 : hostctx::kPage;
	const int32_t r = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), size, align, kWbOnion, phys);
	if (r != 0)
		fprintf(stderr, "host: no direct memory for %#zx bytes (%#x)\n", size, (unsigned)r);
	return r == 0;
}

bool mapDirect(void* at, size_t size, off_t phys)
{
	void* p = at;
	const int32_t r = sceKernelMapDirectMemory(&p, size, kProtRead | kProtWrite, kMapFixed, phys, hostctx::kPage);
	if (r != 0 || p != at)
	{
		fprintf(stderr, "host: mapping %#zx bytes at %p failed (%#x, at %p)\n", size, at, (unsigned)r, p);
		return false;
	}
	return true;
}
} // namespace

bool hostReserveGuest(uint8_t** base)
{
	// (away from libkernel's objects at 0x200000000 and the recompiler's tables at 0x4000000000+)
	void* addr = (void*)0x6000000000ull;
	int32_t r = sceKernelReserveVirtualRange(&addr, 0x100000000ull, kMapFixed, hostctx::kPage);
	if (r != 0)
	{
		fprintf(stderr, "host: reserving 4 GiB at 0x6000000000 failed (%#x), anywhere instead\n", (unsigned)r);
		addr = (void*)0x6000000000ull;
		r = sceKernelReserveVirtualRange(&addr, 0x100000000ull, 0, 0x10000000ull);
	}
	if (r != 0)
	{
		fprintf(stderr, "host: the guest's 4 GiB can't be reserved (%#x)\n", (unsigned)r);
		return false;
	}
	*base = (uint8_t*)addr;
	return true;
}

bool hostMapMirrored(uint8_t* base, const uint32_t* guest, int views, uint32_t size)
{
	// a second mapping of direct memory is refused (EBUSY, 0x80020010) until aliasing is on for the
	// process (as DolphinPS4's MemArenaPS4.cpp found)
	static bool s_aliasing = false;
	if (!s_aliasing && views > 1)
	{
		s_aliasing = true;
		const int32_t r1 = sceKernelEnableDmemAliasing();
		const int32_t r2 = r1 == 0 ? 0 : sceKernelEnableDmemAliasing2();
		fprintf(stderr, "host: direct memory aliasing %#x, aliasing2 %#x\n", (unsigned)r1, (unsigned)r2);
	}
	const size_t n = roundPage(size);
	off_t phys = 0;
	if (!allocDirect(n, &phys))
		return false;
	for (int i = 0; i < views; i++)
		if (!mapDirect(base + guest[i], n, phys))
			return false;
	memset(base + guest[0], 0, n);
	return true;
}

bool hostMapPlain(uint8_t* at, uint32_t size)
{
	const uint32_t view = 0;
	return hostMapMirrored(at, &view, 1, size);
}

bool hostPageMap(void* at)
{
	// (one physical page, mapped wherever a step needs it: a step's page is filled fresh each time)
	static off_t s_phys = -1;
	if (s_phys < 0 && !allocDirect(hostctx::kPage, &s_phys))
		return false;
	if (!mapDirect(at, hostctx::kPage, s_phys))
		return false;
	memset(at, 0, hostctx::kPage);
	return true;
}

void hostPageUnmap(void* at)
{
	sceKernelMunmap(at, hostctx::kPage);
	// (unmapping also dropped the page's reservation: taken back, inaccessible again)
	void* addr = at;
	sceKernelReserveVirtualRange(&addr, hostctx::kPage, kMapFixed, hostctx::kPage);
}

bool hostProtect(void* at, size_t size, bool writable)
{
	return sceKernelMprotect(at, size, writable ? kProtRead | kProtWrite : kProtRead) == 0;
}

#else // Linux

bool hostReserveGuest(uint8_t** base)
{
	void* p = mmap(nullptr, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return false;
	*base = (uint8_t*)p;
	return true;
}

bool hostMapMirrored(uint8_t* base, const uint32_t* guest, int views, uint32_t size)
{
	const int fd = memfd_create("ssa-guest", 0);
	if (fd < 0 || ftruncate(fd, size) != 0)
		return false;
	for (int i = 0; i < views; i++)
	{
		void* p = mmap(base + guest[i], size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
		if (p != base + guest[i])
			return false;
	}
	close(fd);
	return true;
}

bool hostMapPlain(uint8_t* at, uint32_t size)
{
	return mmap(at, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == at;
}

bool hostPageMap(void* at)
{
	return mmap(at, hostctx::kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == at;
}

void hostPageUnmap(void* at)
{
	mmap(at, hostctx::kPage, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
}

bool hostProtect(void* at, size_t size, bool writable)
{
	return mprotect(at, size, writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0;
}
#endif
