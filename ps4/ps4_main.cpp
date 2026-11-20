// Skylanders: Spyro's Adventure on the PS4: the app's entry point. The runtime (recomp/ssa_run.cpp
// and the devices) is the PC host's, unchanged; this sets up what the PC test driver did:
//
// - the log: /data/SSA/ssa.log (stderr), the previous run's kept as ssa-prev.log,
// - settings: /data/SSA/env.txt, one VAR=value per line (# comments), the same switches as on the
//   PC (SSA_SECONDS, SSA_GX_*, SSA_VK_*, ...); defaults below point at the files in /data/SSA; the
//   quick menu's settings in /data/SSA/settings.ini (env.txt still replaces any),
// - guest memory (host.cpp), the recompiler's tables and glue, then ssaRun.
//
// The files in /data/SSA: disc/ (the extracted disc: sys/, files/, ...), ssa16.aot (the translated
// code, AOT format 8, made with recomp/patch-aot-calls.py, -float.py, -mem.py, -expand.py, -mfcr.py and
// a profile-guided build, recomp/pgo.sh), nand/ (setting.txt, SYSCONF, saves), figures/ (Portal figure
// dumps). The one-file package (ps4/build.sh SSA_PKG_FULL) has disc/ and ssa16.aot in /app0, used when
// /data/SSA has none, and seed/ for the first start (seedFromPackage).
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Common/cpu_features.h"
#include "util/MemMapper/MemMapper.h"
#include "host_ctx.h"
#include "settings.h"
#include "ps4_kit.h"
#include "ps4_kit_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <xmmintrin.h>

extern "C" void ps4_boot_trace(const char* line); // ps4-kit: /data/SSA/boot-trace.log

PS4_DIAG_DATA_DIR("/data/SSA");

extern uint8* memory_base;
int ssaRun(PPCInterpreterGlobal_t* global); // ssa_run.cpp
void ps4AudioStart();                        // ps4_audio.cpp: the sound (SSA_AUDIO=0: none)
void ps4ThreadsInit();                       // ps4_threads.cpp: thread placement
void ps4InputStart();                        // ps4_input.cpp: the pad reader (the quick menu, the Wii Remote)
void ssaPinThread(int core, int priority, const char* name);
void PPCRecompiler_initPlatform();
void PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();

extern "C" void ps4_kit_configure_heap(ps4_kit_heap_options* o)
{
	o->log_tag = "ssa";
}

extern "C" int32_t sceKernelHasNeoMode(void); // libkernel: a PS4 Pro
extern "C" int32_t sceKernelIsNeoMode(void);  // ... running the app in the Pro's own mode
void overlayToast(const char* text, int ms);    // overlay.cpp
void overlaySetCpu(unsigned mhz, bool boost);

namespace
{
constexpr const char* kDir = "/data/SSA";

// The one-file package (v00.23): the game's files are in the package too (/app0): disc/ (the extracted
// disc), ssa16.aot (the translated code) and seed/ (the first start's saves, figure and quick-menu
// settings: nand/, figures/spyro.sky, settings.ini). The disc and the translated code in /data/SSA,
// where they were put by hand before (and still may be), come first. What the game writes stays in
// /data/SSA: the first start copies seed/'s parts there, each only if /data/SSA has none.
constexpr const char* kApp = "/app0";

bool exists(const std::string& path)
{
	return access(path.c_str(), F_OK) == 0;
}

bool copyFile(const std::string& from, const std::string& to)
{
	FILE* in = fopen(from.c_str(), "rb");
	if (!in)
		return false;
	FILE* out = fopen(to.c_str(), "wb");
	if (!out)
	{
		fclose(in);
		return false;
	}
	static char buf[1 << 16];
	bool ok = true;
	for (size_t n; (n = fread(buf, 1, sizeof(buf), in)) > 0;)
		if (fwrite(buf, 1, n, out) != n)
		{
			ok = false;
			break;
		}
	fclose(in);
	return fclose(out) == 0 && ok;
}

// a folder and everything in it (the files copied counted in *files)
bool copyTree(const std::string& from, const std::string& to, int* files)
{
	DIR* d = opendir(from.c_str());
	if (!d)
		return false;
	mkdir(to.c_str(), 0777);
	bool ok = true;
	while (const dirent* e = readdir(d))
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		const std::string a = from + "/" + e->d_name, b = to + "/" + e->d_name;
		bool dir = e->d_type == DT_DIR;
		if (e->d_type == DT_UNKNOWN) // (a file system that doesn't say: a folder opens)
			if (DIR* t = opendir(a.c_str()))
			{
				closedir(t);
				dir = true;
			}
		if (dir)
			ok = copyTree(a, b, files) && ok;
		else if (copyFile(a, b))
			++*files;
		else
			ok = false;
	}
	closedir(d);
	return ok;
}

// the first start's saves, figure and settings from the package's seed/
void seedFromPackage()
{
	const std::string seed = std::string(kApp) + "/seed";
	if (!exists(seed))
		return;
	// the NAND (the saves): copied whole to nand.new, then renamed (an interrupted copy is done again)
	if (!exists("/data/SSA/nand") && exists(seed + "/nand"))
	{
		int files = 0;
		if (copyTree(seed + "/nand", "/data/SSA/nand.new", &files) && rename("/data/SSA/nand.new", "/data/SSA/nand") == 0)
			fprintf(stderr, "ssa: first start: the saves (NAND, %d files) from the package\n", files);
		else
			fprintf(stderr, "ssa: first start: copying the package's NAND FAILED (%d files copied)\n", files);
	}
	for (const char* f : {"figures/spyro.sky", "settings.ini"})
	{
		const std::string to = std::string(kDir) + "/" + f;
		if (!exists(to) && exists(seed + "/" + f))
			fprintf(stderr, "ssa: first start: %s from the package%s\n", f, copyFile(seed + "/" + f, to) ? "" : " FAILED");
	}
}

void loadSettings()
{
	// defaults first (env.txt may replace any)
	setenv("SSA_RUN", "1", 1);
	// the disc: /data/SSA's if it's there (put there by hand), else the package's
	const std::string disc = exists("/data/SSA/disc/sys/main.dol") ? std::string(kDir) + "/disc" : std::string(kApp) + "/disc";
	setenv("SSA_DOL", (disc + "/sys/main.dol").c_str(), 1);
	// (format 8 since v00.17, recomp/patch-aot-gp.py; ssa9.aot since v00.19: the same format, translated
	// with the registers kept across calls, recomp/patch-aot-calls.py; ssa15.aot since v00.20: single
	// floats as floats, plain guest memory accesses, mfcr in a few instructions (patch-aot-float.py,
	// -mem.py, -mfcr.py); ssa16.aot since v00.22: the register save/restore helpers expanded in place
	// (patch-aot-expand.py), built with a profile of the PC's scripted run (pgo.sh) - ssa15.aot, ssa9.aot
	// and ssa8.aot (v00.17/18's) still work: SSA_AOT=/data/SSA/ssa15.aot; ssa7.aot is v00.15/16's
	// format 7 and ssa.aot v00.14's format 6, which this can't use)
	setenv("SSA_AOT", "/data/SSA/ssa16.aot", 1); // (else the package's: main)
	setenv("SSA_DISC", disc.c_str(), 1);
	setenv("SSA_NAND", "/data/SSA/nand", 1);
	setenv("SSA_SECONDS", "2000000000", 1);
	setenv("SSA_VK_ALL", "1", 1);
	setenv("SSA_GX_THREAD", "1", 1); // (the renderer on its own thread; SSA_GX_THREAD=0: on the game's)
	setenv("SSA_GX_WORKERS", "3", 1); // (vertex fetch on three more cores; 0: on the GX thread)
	setenv("SSA_PORTAL", "/data/SSA/figures/spyro.sky=16", 1);
	setenv("SSA_FONTS", "/app0/fonts", 1); // (the quick menu's Nunito, in the package)
	// the quick menu's settings (settings.ini, which it writes): the ones that take effect at the start
	// become the switches above (env.txt, read after, still replaces any)
	ssa::settingsLoad("/data/SSA/settings.ini");
	setenv("SSA_GX_WORKERS", ssa::settingValue("workers"), 1);
	setenv("SSA_GX_THREAD", ssa::setting("gxthread") == 0 ? "1" : "0", 1);
	FILE* f = fopen("/data/SSA/env.txt", "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		char* p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		size_t n = strlen(p);
		while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
			p[--n] = 0;
		if (!*p || *p == '#')
			continue;
		char* eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = 0;
		// "VAR=" alone: the default taken out
		if (eq[1])
			setenv(p, eq + 1, 1);
		else
			unsetenv(p);
		fprintf(stderr, "ssa: env.txt: %s=%s\n", p, eq + 1);
	}
	fclose(f);
}
} // namespace

int main()
{
	mkdir(kDir, 0777);
	mkdir("/data/SSA/figures", 0777);
	mkdir("/data/SSA/frames", 0777); // (SSA_FRAMES captures)
	rename("/data/SSA/ssa.log", "/data/SSA/ssa-prev.log");
	// the log: stderr's descriptor pointed at the file (stderr stays unbuffered, every line lands at
	// once, also right before a crash or an exit)
	const int fd = open("/data/SSA/ssa.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
	{
		ps4_boot_trace("ssa: /data/SSA/ssa.log can't be opened");
		return 1;
	}
	dup2(fd, 2);
	close(fd);
	fprintf(stderr, "ssa: Skylanders: Spyro's Adventure, PS4 runtime " SSA_PS4_VERSION "\n");
	seedFromPackage();
	loadSettings();
	// (the translated code not in /data/SSA: the package's; neither there: an earlier one, same format)
	for (const char* older : {"/app0/ssa16.aot", "/data/SSA/ssa15.aot", "/data/SSA/ssa9.aot", "/data/SSA/ssa8.aot"})
		if (access(getenv("SSA_AOT"), R_OK) != 0 && access(older, R_OK) == 0)
		{
			fprintf(stderr, "ssa: no %s: %s instead\n", getenv("SSA_AOT"), older);
			setenv("SSA_AOT", older, 1);
		}
	fprintf(stderr, "ssa: disc %s, translated code %s\n", getenv("SSA_DISC"), getenv("SSA_AOT"));
	// the cores' clock. This is a PS4 app, not one for the Pro's own mode (the kit's RADV knows only the
	// PS4's GPU, make-pkg.sh): a PS4 Pro runs it at 2.13 GHz with Boost Mode on (Settings > System), at
	// 1.6 GHz without - and the game thread's speed is the frame rate
	{
		const unsigned mhz = ps4_kit_cpu_mhz();
		const bool pro = sceKernelHasNeoMode() != 0, proMode = sceKernelIsNeoMode() != 0;
		const bool boost = pro && !proMode && mhz >= 1900; // (~2080-2130 with it, ~1585-1600 without)
		fprintf(stderr, "ssa: CPU %u MHz: %s\n", mhz,
			!pro ? "a PS4" : proMode ? "a PS4 Pro in its own mode" : boost ? "a PS4 Pro, Boost Mode on" : "a PS4 Pro, Boost Mode OFF");
		overlaySetCpu(mhz, boost);
		if (pro && !proMode && !boost)
			overlayToast("PS4 Pro: turn on Boost Mode (Settings > System) - the game runs about 1/3 faster", 10000);
	}

	// the Jaguar: AVX, MOVBE, LZCNT, SSE4.1, no BMI2/AVX2 (the translated code was built for btver2)
	g_CPUFeatures.x86.bmi2 = false;
	g_CPUFeatures.x86.avx2 = false;
	g_CPUFeatures.x86.fastPext = false;
	g_CPUFeatures.x86.avx = true;
	g_CPUFeatures.x86.movbe = true;
	g_CPUFeatures.x86.lzcnt = true;
	g_CPUFeatures.x86.sse4_1 = true;
	g_CPUFeatures.x86.ssse3 = true;

	if (!hostReserveGuest(&memory_base))
	{
		ps4_boot_trace("ssa: the guest's 4 GiB can't be reserved (ssa.log)");
		return 1;
	}
	fprintf(stderr, "ssa: guest memory at %p\n", memory_base);
	ps4_boot_trace("ssa: guest memory reserved");
	auto* global = (PPCInterpreterGlobal_t*)calloc(1, 0x10000);
	ppcRecompilerInstanceData = PPCRecompiler_reserveInstanceData();
	if (!ppcRecompilerInstanceData)
	{
		fprintf(stderr, "ssa: the recompiler's instance data can't be reserved\n");
		return 1;
	}
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom),
		sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW,
		true);
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
	// the jump table's block for guest address 0: slot 0 is where translated code leaves to the
	// dispatcher (the bridge's jmp [r15+table]); MEM1's blocks come in ssaRun
	PPCRecompiler_allocateRange(0, 0x1000);
	PPCRecompiler_initPlatform();
	_mm_setcsr(0x9FC0);
	// SSA_PROFILE=1 (env.txt): the kit's 1 kHz sampler on this thread (the game, devices and renderer
	// all run on it), /data/SSA/samples.log every 10 s; scripts/profile.py names the samples (the
	// translated code with --regions: ssa.aot's image address is in boot-trace.log's mmap #1)
	if (const char* p = getenv("SSA_PROFILE"); p && atoi(p))
	{
		ps4_sampler_set_code_dumps(0);
		ps4_sampler_register_thread("ssa");
		ps4_sampler_start();
		fprintf(stderr, "ssa: the sampler is on (samples.log)\n");
	}
	ps4ThreadsInit();
	ssaPinThread(0, 650, "ssa"); // (the game thread: ps4_threads.cpp has the layout)
	ps4AudioStart();
	ps4InputStart();
	ps4_boot_trace("ssa: running");
	const int r = ssaRun(global);
	fprintf(stderr, "ssa: the run ended (%d)\n", r);
	char line[64];
	snprintf(line, sizeof(line), "ssa: the run ended (%d, see ssa.log)", r);
	ps4_boot_trace(line);
	fflush(stderr);
	ps4_kit_exit(r);
}
