// Where the app's busy threads run. The PS4 runs threads by priority alone, with no turns among
// threads of one priority (ps4-kit's notes): two busy threads sharing a core take turns only when one
// waits, so each busy thread gets a core of its own and a priority above the default (700):
//   core 0  the game thread (the translated PowerPC code, the devices)   650
//   core 1  the GX thread (FIFO decode, draw recording, presenting)       650
//   2-4     the vertex fetch workers (SSA_GX_WORKERS, default 3)          660
//   core 5  the sound (ps4_audio.cpp: mostly waiting on the port)         400
//           and the DS4 reader (ps4_input.cpp: a read every 4 ms)          600
//   core 6  left to the system and the floating threads (RADV's submits, the logger, the sampler)
#include "ps4_kit.h"
#include "ps4_kit_diag.h"
#include <stdint.h>
#include <stdio.h>

extern "C" int ssa_scePthreadSetaffinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
extern "C" void* ssa_scePthreadSelf(void) __asm__("scePthreadSelf");
extern void (*g_ssaPinThread)(int core, int priority, const char* name); // gx.cpp

void ssaPinThread(int core, int priority, const char* name)
{
	const int r = ssa_scePthreadSetaffinity(ssa_scePthreadSelf(), 1ull << (core & 7));
	ps4_kit_set_thread_priority(priority);
	ps4_watch_thread(name);
	fprintf(stderr, "threads: %s on core %d, priority %d%s\n", name, core, ps4_kit_thread_priority(), r ? " (affinity refused)" : "");
}

void ps4ThreadsInit()
{
	g_ssaPinThread = ssaPinThread;
}
