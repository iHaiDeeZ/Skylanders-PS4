// The host's signal context, the same on both hosts: the faulting instruction's address, the
// flags (the trap flag for single steps) and the general registers by x86 encoding number
// (0 rax, 1 rcx, 2 rdx, 3 rbx, 4 rsp, 5 rbp, 6 rsi, 7 rdi, 8-15 r8-r15).
// Linux: glibc's gregs. PS4 (SSA_PS4): FreeBSD's mcontext at byte 0x40 (ps4-kit's libc overlay).
#pragma once
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>

namespace hostctx
{
#ifdef SSA_PS4
constexpr uintptr_t kPage = 0x4000; // (direct memory maps in 16 KiB pages)
inline uint64_t& rip(void* c) { return *(uint64_t*)&((ucontext_t*)c)->uc_mcontext.mc_rip; }
inline uint64_t& flags(void* c) { return *(uint64_t*)&((ucontext_t*)c)->uc_mcontext.mc_rflags; }
inline uint64_t& reg(void* c, int x86)
{
	auto& m = ((ucontext_t*)c)->uc_mcontext;
	long* r[16] = {&m.mc_rax, &m.mc_rcx, &m.mc_rdx, &m.mc_rbx, &m.mc_rsp, &m.mc_rbp, &m.mc_rsi, &m.mc_rdi,
		&m.mc_r8, &m.mc_r9, &m.mc_r10, &m.mc_r11, &m.mc_r12, &m.mc_r13, &m.mc_r14, &m.mc_r15};
	return *(uint64_t*)r[x86 & 15];
}
#else
constexpr uintptr_t kPage = 0x1000;
inline uint64_t& rip(void* c) { return *(uint64_t*)&((ucontext_t*)c)->uc_mcontext.gregs[REG_RIP]; }
inline uint64_t& flags(void* c) { return *(uint64_t*)&((ucontext_t*)c)->uc_mcontext.gregs[REG_EFL]; }
inline uint64_t& reg(void* c, int x86)
{
	static const int kGreg[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
		REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
	return *(uint64_t*)&((ucontext_t*)c)->uc_mcontext.gregs[kGreg[x86 & 15]];
}
#endif
} // namespace hostctx

// host.cpp: guest memory on this host
bool hostPageMap(void* at);   // a zeroed read/write page (kPage) at an inaccessible guest page
void hostPageUnmap(void* at); // and back to inaccessible
bool hostReserveGuest(uint8_t** base); // 4 GiB of guest address space, inaccessible
// one block seen at several guest addresses (the Wii's physical, cached and uncached views)
bool hostMapMirrored(uint8_t* base, const uint32_t* guest, int views, uint32_t size);
bool hostMapPlain(uint8_t* at, uint32_t size); // zeroed read/write memory at a guest address
bool hostProtect(void* at, size_t size, bool writable); // (SSA_WWATCH: read-only and back)
