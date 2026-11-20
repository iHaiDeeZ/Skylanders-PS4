// Skylanders runtime: a GX recording (SSA_GXREC) drawn again on the host, in seconds, for renderer
// work. gx_replay <recording> <frame dir> [VAR=value...]: every frame with draws is written as a
// PNG; the switches are the runtime's (SSA_VK_DEBUG, SSA_VK_TRACE, SSA_VK_FRONT, ...).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>

uint8_t* memory_base;
int gxReplay(const char* path);
void gxReport();

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: gx_replay <recording> <frame dir> [VAR=value...]\n");
		return 1;
	}
	for (int i = 3; i < argc; i++)
		if (char* eq = strchr(argv[i], '='))
		{
			*eq = 0;
			setenv(argv[i], eq + 1, 1);
		}
	setenv("SSA_FRAMES", argv[2], 1);
	setenv("SSA_FRAMES_EVERY", "1", 1);
	setenv("SSA_VK_ALL", "1", 1);
	setenv("SSA_GX_LOG", "1", 1);
	// guest RAM where the runtime has it: MEM1 at +0x80000000, MEM2 at +0x90000000
	void* p = mmap(nullptr, 0x94000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return 2;
	memory_base = (uint8_t*)p;
	mprotect(memory_base + 0x80000000ull, 0x01800000, PROT_READ | PROT_WRITE);
	mprotect(memory_base + 0x90000000ull, 0x04000000, PROT_READ | PROT_WRITE);
	// (SSA_REPLAY_LOOPS=n: the recording n times over, for runs longer than it: the quick menu's pictures)
	const int loops = getenv("SSA_REPLAY_LOOPS") ? atoi(getenv("SSA_REPLAY_LOOPS")) : 1;
	int r = 0;
	for (int i = 0; i < loops && !r; i++)
		r = gxReplay(argv[1]);
	if (r)
		fprintf(stderr, "gx_replay: %s: can't read (%d)\n", argv[1], r);
	gxReport();
	return r;
}
