// jittest: bytes allocated through operator new (the recompiler's containers), current and peak,
// so JT_RPX can report what each function's compile needed at most.
#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

std::atomic<long long> g_jtAllocated{0};
std::atomic<long long> g_jtPeak{0};

static void* track(void* p)
{
	if (!p)
		throw std::bad_alloc();
	const long long now = g_jtAllocated.fetch_add((long long)malloc_usable_size(p)) + (long long)malloc_usable_size(p);
	long long peak = g_jtPeak.load();
	while (now > peak && !g_jtPeak.compare_exchange_weak(peak, now))
	{
	}
	return p;
}

static void untrack(void* p)
{
	if (p)
	{
		g_jtAllocated.fetch_sub((long long)malloc_usable_size(p));
		free(p);
	}
}

void* operator new(std::size_t n) { return track(malloc(n ? n : 1)); }
void* operator new[](std::size_t n) { return track(malloc(n ? n : 1)); }
void* operator new(std::size_t n, std::align_val_t a) { return track(aligned_alloc((size_t)a, ((n ? n : 1) + (size_t)a - 1) & ~((size_t)a - 1))); }
void* operator new[](std::size_t n, std::align_val_t a) { return track(aligned_alloc((size_t)a, ((n ? n : 1) + (size_t)a - 1) & ~((size_t)a - 1))); }
void operator delete(void* p) noexcept { untrack(p); }
void operator delete[](void* p) noexcept { untrack(p); }
void operator delete(void* p, std::size_t) noexcept { untrack(p); }
void operator delete[](void* p, std::size_t) noexcept { untrack(p); }
void operator delete(void* p, std::align_val_t) noexcept { untrack(p); }
void operator delete[](void* p, std::align_val_t) noexcept { untrack(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { untrack(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { untrack(p); }
