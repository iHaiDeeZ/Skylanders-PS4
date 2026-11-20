// Skylanders runtime: the Wii's IPC hardware (Hollywood 0xCD000000) and a high-level IOS behind it.
//
// IPC, as Dolphin's WII_IPC.cpp / IOS.cpp have it:
//   PPCMSG (0xCD000000)  the request's physical address
//   PPCCTRL (0xCD000004) X1 (bit 0: execute), Y2 (bit 1: IOS acknowledged), Y1 (bit 2: reply ready),
//                        X2 (bit 3), IY1/IY2 (bits 4/5: interrupt on Y1/Y2). Writing Y1/Y2 as 1 clears
//                        them, and fires the IPC interrupt when its enable is set.
//   ARMMSG (0xCD000008)  the replied request's physical address
//   PPC IRQ flag/mask (0xCD000030/34): IPC = bit 30; flag & mask drives PI cause 0x4000.
// IOS takes a request only when Y1 and Y2 are clear and no IPC interrupt is pending: it acks it (Y2),
// runs it, and queues the reply; a reply goes out (ARMMSG, Y1) under the same condition.
//
// A request in memory (big-endian, physical address): command (1 open, 2 close, 3 read, 4 write,
// 5 seek, 6 ioctl, 7 ioctlv), result, fd, then arguments. The reply writes the result, the command
// at +8 and IPC_REPLY (8) as the command. Physical addresses are guest addresses here (MEM1/MEM2 are
// mapped at their physical addresses).
//
// The IOS behind it, v1: devices open (the ones a Wii has; others: -6), files on a host NAND folder
// (SSA_NAND; missing: -106), every request logged. Requests IOS answers only later (STM's event hook)
// stay pending. Everything else succeeds with 0 for now: the log shows what the game needs next.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <deque>
#include <map>
#include <string>
#include <filesystem>
#include <optional>
#include <vector>
#include <algorithm>

extern uint8_t* memory_base;
void discReadRaw(uint64_t offset, uint32_t length, uint8_t* out); // disc.cpp
void discReadPartition(uint64_t offset, uint32_t length, uint8_t* out);
std::vector<uint8_t> discTmd();
std::vector<uint8_t> discTicket();
extern uint64_t (*g_mmioTimeBase)(); // mmio.cpp: the runtime's time base
void iosReplyLater(uint32_t req, int32_t result); // below
std::optional<int32_t> btIoctlv(uint32_t req, uint32_t ioctl, uint32_t nIn, uint32_t nIo, uint32_t vec); // bt.cpp
std::optional<int32_t> hidIoctl(uint32_t req, uint32_t ioctl, uint32_t in, uint32_t inLen, uint32_t out, uint32_t outLen); // portal.cpp

namespace
{
uint32_t be32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(memory_base + a)); }
void put32(uint32_t a, uint32_t v) { *(uint32_t*)(memory_base + a) = __builtin_bswap32(v); }

struct Ctrl
{
	bool X1 = false, X2 = false, Y1 = false, Y2 = false, IY1 = false, IY2 = false;
	uint32_t ppc() const { return (IY2 << 5) | (IY1 << 4) | (X2 << 3) | (Y1 << 2) | (Y2 << 1) | (uint32_t)X1; }
} s_ctrl;
uint32_t s_ppcMsg = 0, s_armMsg = 0, s_irqFlags = 0, s_irqMask = 0;
constexpr uint32_t kIrqIpc = 1u << 30;
std::deque<uint32_t> s_requests, s_replies;

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...)
{
	char line[512];
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	if (n < 0)
		return;
	n = n > (int)sizeof(line) - 2 ? (int)sizeof(line) - 2 : n;
	line[n++] = '\n';
	write(2, line, n);
}

std::string guestString(uint32_t a, size_t max = 256)
{
	std::string s;
	for (size_t i = 0; i < max && memory_base[a + i]; i++)
		s += (char)memory_base[a + i];
	return s;
}

// ---- the IOS kernel (high level) ----
enum : int32_t
{
	kOk = 0,
	kIpcEnoent = -6,
	kFsEnoent = -106,
	kFsEinval = -101,
};

struct Fd
{
	std::string path;
	bool device = false;
	FILE* file = nullptr;
};
std::map<uint32_t, Fd> s_fds;


const char* const kDevices[] = {"/dev/es", "/dev/fs", "/dev/stm/immediate", "/dev/stm/eventhook", "/dev/di", "/dev/usb/oh1/57e/305",
	"/dev/usb/hid", "/dev/usb/oh0", "/dev/usb/ven", "/dev/sdio/slot0", "/dev/net/kd/request", "/dev/net/kd/time", "/dev/net/ncd/manage",
	"/dev/net/ip/top", "/dev/net/wd/command", "/dev/net/ssl", "/dev/wl0", "/dev/usb/kbd", "/dev/aes", "/dev/sha", "/dev/boot2", "/dev/flash",
	"/dev/usb/wfssrv", "/dev/usb/oh1"};

// ---- /dev/di: the disc drive (disc.cpp has the disc) ----
// An ioctl's input: the command word, then its arguments (for reads: length in bytes, offset >> 2).
// Results: 1 success, 2 drive error (Dolphin's DIResult). The cover is closed with a disc inside.
// the data partition is open when the game starts: the boot program and apploader opened it (Dolphin
// BS2: ChangePartition); the game reads it without opening it again
bool s_partitionOpen = true;
uint32_t s_diError = 0;                         // the drive's last error (RequestError)
constexpr uint64_t kDiscEnd = 0x118240000ull;   // a single-layer disc's end (Dolphin's m_disc_end_offset)
int s_discReads = 0;

std::optional<int32_t> diIoctl(uint32_t ioctl, uint32_t in, uint32_t out, uint32_t outLen);

// The drive works through its commands one at a time and takes time: a read's data lands in memory
// when the transfer ends (8 MB/s plus 0.2 ms), other commands take 50 us. Games rely on it: the
// streaming loader reads every chunk into the same 32 KB buffer and copies it out meanwhile; a read
// completed at once overwrote a chunk before it was copied (corrupted archive data).

struct DiJob
{
	uint32_t req, ioctl, in, out, outLen;
	uint64_t due = 0;
	bool started = false;
};
std::deque<DiJob> s_diJobs;

uint64_t diLatency(const DiJob& j)
{
	constexpr uint64_t kTicksPerSecond = 60750000;
	if (j.ioctl == 0x71 || j.ioctl == 0x8D)
	{
		const uint64_t bytes = be32(j.in + 4);
		return kTicksPerSecond / 5000 + bytes * kTicksPerSecond / 8000000;
	}
	return kTicksPerSecond / 20000;
}

void diPoll()
{
	if (s_diJobs.empty() || !g_mmioTimeBase)
		return;
	const uint64_t now = g_mmioTimeBase();
	DiJob& j = s_diJobs.front();
	if (!j.started)
	{
		j.started = true;
		j.due = now + diLatency(j);
	}
	if (now < j.due)
		return;
	const DiJob done = j;
	s_diJobs.pop_front();
	if (auto result = diIoctl(done.ioctl, done.in, done.out, done.outLen))
		::iosReplyLater(done.req, *result);
}

std::optional<int32_t> diIoctl(uint32_t ioctl, uint32_t in, uint32_t out, uint32_t outLen)
{
	const uint32_t a1 = be32(in + 4), a2 = be32(in + 8);
	auto readLog = [&](const char* what, uint64_t off, uint32_t len) {
		if (s_discReads++ < 300)
			log("ios: di %s %llx +%x -> %08x", what, (unsigned long long)off, len, out);
	};
	switch (ioctl)
	{
	case 0x12: // Inquiry (Dolphin's drive)
		memset(memory_base + out, 0, outLen);
		put32(out, 0x00000002);
		put32(out + 4, 0x20060526);
		put32(out + 8, 0x41000000);
		return 1;
	case 0x70: // ReadDiskID
		discReadRaw(0, 0x20, memory_base + out);
		log("ios: di ReadDiskID -> %.6s", (const char*)(memory_base + out));
		return 1;
	case 0x71: // Read (the open partition's data)
		if (!s_partitionOpen)
			return 2;
		readLog("read", (uint64_t)a2 << 2, a1);
		discReadPartition((uint64_t)a2 << 2, a1, memory_base + out);
		return 1;
	case 0x8D: // UnencryptedRead
		readLog("raw read", (uint64_t)a2 << 2, a1);
		// past the end of a single-layer disc: the drive's out-of-bounds error, as a pressed disc
		// gives (the game's "error #001" check reads just past it: a burned disc would succeed)
		if (((uint64_t)a2 << 2) + a1 > kDiscEnd)
		{
			s_diError = 0x052100;
			log("ios: di raw read past the disc's end: drive error (block out of bounds)");
			return 2;
		}
		discReadRaw((uint64_t)a2 << 2, a1, memory_base + out);
		return 1;
	case 0x79: // WaitForCoverClose: answered when the cover closes (never)
		log("ios: di WaitForCoverClose (pending)");
		return std::nullopt;
	case 0xA4: // ReportKey: retail drives reject it (games check that they run on retail hardware)
		s_diError = 0x052000; // invalid command
		log("ios: di ReportKey: drive error (invalid command), as a retail drive");
		return 2;
	case 0xE0: // RequestError: the last error (drive state 0: ready), then none
		put32(out, s_diError);
		log("ios: di RequestError -> %06x", s_diError);
		s_diError = 0;
		return 1;
	case 0x7A: // GetCoverRegister: closed
	case 0x95: // GetStatusRegister
	case 0x96: // GetControlRegister
		put32(out, 0);
		return 1;
	case 0x88: // GetCoverStatus: a disc inside
		put32(out, 2);
		return 1;
	case 0x8C: // ClosePartition
		s_partitionOpen = false;
		return 1;
	default:
		log("ios: di ioctl 0x%x (%08x %08x): 1", ioctl, a1, a2);
		return 1;
	}
}

std::string nandRoot()
{
	const char* r = getenv("SSA_NAND");
	return r ? r : "nand";
}

int32_t iosOpen(const std::string& path, uint32_t mode)
{
	Fd fd;
	fd.path = path;
	if (path.rfind("/dev/", 0) == 0)
	{
		bool known = false;
		for (const char* d : kDevices)
			known |= path == d;
		if (!known)
			return kIpcEnoent;
		fd.device = true;
	}
	else
	{
		const std::string host = nandRoot() + path;
		fd.file = fopen(host.c_str(), (mode & 2) ? "r+b" : "rb");
		if (!fd.file)
			return kFsEnoent;
	}
	// the lowest free descriptor, as IOS hands them out (32 of them): SDK code indexes per-fd tables
	// with it (ever-growing numbers read past them)
	uint32_t n = 0;
	while (s_fds.count(n))
		n++;
	if (n >= 32)
	{
		if (fd.file)
			fclose(fd.file);
		return -22; // IPC_EMAX: no descriptors left
	}
	s_fds[n] = fd;
	return (int32_t)n;
}


// ---- /dev/fs (after Dolphin's FS device): the NAND as a host folder (SSA_NAND). ISFS parameters:
// uid u32, gid u16, path[64], owner/group/other modes, attribute (74 bytes) ----
constexpr int32_t kFsEexist = -105;
constexpr uint32_t kClusterSize = 0x4000;

bool nandPathOk(const std::string& p)
{
	return !p.empty() && p[0] == '/' && p.find("..") == std::string::npos;
}

// clusters and inodes under a host path (GetUsage)
void nandUsage(const std::filesystem::path& host, uint32_t& clusters, uint32_t& inodes)
{
	std::error_code ec;
	inodes++;
	if (std::filesystem::is_directory(host, ec))
	{
		for (const auto& e : std::filesystem::directory_iterator(host, ec))
			nandUsage(e.path(), clusters, inodes);
	}
	else
		clusters += (uint32_t)((std::filesystem::file_size(host, ec) + kClusterSize - 1) / kClusterSize);
}

std::optional<int32_t> fsIoctl(uint32_t ioctl, uint32_t in, uint32_t inLen, uint32_t out, uint32_t outLen)
{
	std::error_code ec;
	switch (ioctl)
	{
	case 2: // GetStats: cluster size, free/used/bad/reserved clusters, free/used inodes
	{
		if (outLen < 28)
			return kFsEinval;
		const uint32_t v[7] = {kClusterSize, 0x4000, 0x0400, 0, 0x10, 0x1000, 0x0200};
		for (int i = 0; i < 7; i++)
			put32(out + 4 * i, v[i]);
		log("ios: fs GetStats");
		return kOk;
	}
	case 3: // CreateDirectory
	case 9: // CreateFile
	{
		const std::string path = guestString(in + 6, 64);
		if (!nandPathOk(path))
			return kFsEinval;
		const std::filesystem::path host = nandRoot() + path;
		int32_t r = kOk;
		if (std::filesystem::exists(host, ec))
			r = kFsEexist;
		else if (!std::filesystem::is_directory(host.parent_path(), ec))
			r = kFsEnoent;
		else if (ioctl == 3)
			std::filesystem::create_directory(host, ec);
		else if (FILE* f = fopen(host.c_str(), "wb"))
			fclose(f);
		else
			r = kFsEinval;
		log("ios: fs %s %s -> %d", ioctl == 3 ? "CreateDirectory" : "CreateFile", path.c_str(), r);
		return r;
	}
	case 5: // SetAttribute
	case 6: // GetAttribute
	{
		const std::string path = guestString(ioctl == 6 ? in : in + 6, 64);
		if (!nandPathOk(path))
			return kFsEinval;
		const bool exists = std::filesystem::exists(nandRoot() + path, ec);
		if (ioctl == 6 && exists && out && outLen >= 74)
		{
			memset(memory_base + out, 0, 74);
			put32(out, 0x1000);
			memory_base[out + 4] = 0x35; // gid: the maker code ("52")
			memory_base[out + 5] = 0x32;
			memcpy(memory_base + out + 6, path.c_str(), std::min<size_t>(path.size(), 63));
			memory_base[out + 70] = 3; // owner/group/other: read + write
			memory_base[out + 71] = 3;
			memory_base[out + 72] = 3;
		}
		log("ios: fs %s %s -> %d", ioctl == 6 ? "GetAttribute" : "SetAttribute", path.c_str(), exists ? kOk : kFsEnoent);
		return exists ? kOk : kFsEnoent;
	}
	case 7: // Delete
	{
		const std::string path = guestString(in, 64);
		if (!nandPathOk(path))
			return kFsEinval;
		const std::filesystem::path host = nandRoot() + path;
		if (!std::filesystem::exists(host, ec))
			return kFsEnoent;
		std::filesystem::remove_all(host, ec);
		log("ios: fs Delete %s", path.c_str());
		return kOk;
	}
	case 8: // Rename
	{
		const std::string from = guestString(in, 64), to = guestString(in + 64, 64);
		if (!nandPathOk(from) || !nandPathOk(to))
			return kFsEinval;
		const std::filesystem::path a = nandRoot() + from, b = nandRoot() + to;
		if (!std::filesystem::exists(a, ec))
			return kFsEnoent;
		if (std::filesystem::exists(b, ec))
			std::filesystem::remove_all(b, ec);
		std::filesystem::rename(a, b, ec);
		log("ios: fs Rename %s -> %s", from.c_str(), to.c_str());
		return ec ? kFsEinval : kOk;
	}
	case 0xA: // SetFileVersionControl
	case 0xD: // Shutdown
		return kOk;
	default:
		log("ios: fs ioctl 0x%x (not handled)", ioctl);
		return kFsEinval;
	}
}

// ReadDirectory (4: count, or names + count) and GetUsage (0xC: clusters, inodes)
std::optional<int32_t> fsIoctlv(uint32_t ioctl, uint32_t nIn, uint32_t nIo, uint32_t vec)
{
	auto vecAddr = [&](uint32_t i) { return be32(vec + i * 8); };
	auto vecLen = [&](uint32_t i) { return be32(vec + i * 8 + 4); };
	std::error_code ec;
	const std::string path = nIn >= 1 ? guestString(vecAddr(0), 64) : "";
	if (!nandPathOk(path))
		return kFsEinval;
	const std::filesystem::path host = nandRoot() + path;
	if (ioctl == 0xC && nIo >= 2)
	{
		if (!std::filesystem::exists(host, ec))
			return kFsEnoent;
		uint32_t clusters = 0, inodes = 0;
		nandUsage(host, clusters, inodes);
		put32(vecAddr(nIn), clusters);
		put32(vecAddr(nIn + 1), inodes);
		log("ios: fs GetUsage %s -> %u clusters, %u inodes", path.c_str(), clusters, inodes);
		return kOk;
	}
	if (ioctl == 4)
	{
		if (!std::filesystem::is_directory(host, ec))
			return kFsEnoent;
		std::vector<std::string> names;
		for (const auto& e : std::filesystem::directory_iterator(host, ec))
			names.push_back(e.path().filename().string());
		if (nIn == 1 && nIo == 1)
		{
			put32(vecAddr(1), (uint32_t)names.size());
			log("ios: fs ReadDirectory %s -> %zu entries", path.c_str(), names.size());
			return kOk;
		}
		if (nIn == 2 && nIo == 2)
		{
			const uint32_t max = be32(vecAddr(1));
			uint32_t at = vecAddr(2), count = 0;
			const uint32_t end = at + vecLen(2);
			for (const std::string& n : names)
			{
				if (count >= max || at + n.size() + 1 > end)
					break;
				memcpy(memory_base + at, n.c_str(), n.size() + 1);
				at += (uint32_t)n.size() + 1;
				count++;
			}
			put32(vecAddr(3), count);
			log("ios: fs ReadDirectory %s -> %u names", path.c_str(), count);
			return kOk;
		}
	}
	log("ios: fs ioctlv 0x%x (not handled)", ioctl);
	return kFsEinval;
}

// how often a network device was opened or closed (0: not one). The game polls the network's state by
// opening and closing /dev/net/ip/top, kd/request and ncd/manage, 8 requests a loop, many loops a
// second: a log line each was ~280 writes to the log a second (v00.21). The first 20, then every 1000th.
uint32_t netCalls(const char* path)
{
	if (strncmp(path, "/dev/net/", 9) != 0)
		return 0;
	static std::map<std::string, uint32_t> s_counts;
	return ++s_counts[path];
}

bool logCall(uint32_t n)
{
	return n <= 20 || n % 1000 == 0;
}

// a request: its result, or nothing if IOS answers it later
std::optional<int32_t> iosRun(uint32_t req)
{
	const uint32_t cmd = be32(req), fdn = be32(req + 8);
	auto it = s_fds.find(fdn);
	Fd* fd = it == s_fds.end() ? nullptr : &it->second;
	const char* name = fd ? fd->path.c_str() : "?";
	switch (cmd)
	{
	case 1: // open
	{
		const std::string path = guestString(be32(req + 0xC));
		const int32_t r = iosOpen(path, be32(req + 0x10));
		const uint32_t n = netCalls(path.c_str());
		if (logCall(n))
			log("ios: open %s mode %u -> %d%s", path.c_str(), be32(req + 0x10), r, n > 20 ? " (1000 more opens/closes of it)" : "");
		return r;
	}
	case 2: // close
	{
		const uint32_t n = netCalls(name);
		if (logCall(n))
			log("ios: close %u (%s)%s", fdn, name, n > 20 ? " (1000 more opens/closes of it)" : "");
	}
		if (fd && fd->file)
			fclose(fd->file);
		s_fds.erase(fdn);
		return kOk;
	case 3: // read
	case 4: // write
	{
		const uint32_t buf = be32(req + 0xC), len = be32(req + 0x10);
		if (!fd)
			return kFsEinval;
		if (fd->file)
		{
			const size_t n = cmd == 3 ? fread(memory_base + buf, 1, len, fd->file) : fwrite(memory_base + buf, 1, len, fd->file);
			log("ios: %s %s %u bytes -> %zu", cmd == 3 ? "read" : "write", name, len, n);
			return (int32_t)n;
		}
		log("ios: %s device %s %u bytes (not handled)", cmd == 3 ? "read" : "write", name, len);
		return kOk;
	}
	case 5: // seek
	{
		const int32_t off = (int32_t)be32(req + 0xC);
		const uint32_t whence = be32(req + 0x10);
		if (fd && fd->file && fseek(fd->file, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END) == 0)
		{
			log("ios: seek %s %d (%u) -> %ld", name, off, whence, ftell(fd->file));
			return (int32_t)ftell(fd->file);
		}
		log("ios: seek %s %d (%u) failed", name, off, whence);
		return kFsEinval;
	}
	case 6: // ioctl
	{
		const uint32_t ioctl = be32(req + 0xC), in = be32(req + 0x10), inLen = be32(req + 0x14), out = be32(req + 0x18), outLen = be32(req + 0x1C);
		if (fd && fd->path == "/dev/di")
		{
			s_diJobs.push_back({req, ioctl, in, out, outLen});
			return std::nullopt; // (answered by diPoll when the drive is done)
		}
		// the NAND filesystem, and a file's stats (GetFileStats: size, position)
		if (fd && fd->path == "/dev/fs")
			return fsIoctl(ioctl, in, inLen, out, outLen);
		if (fd && fd->file && ioctl == 0xB && outLen >= 8)
		{
			const long pos = ftell(fd->file);
			fseek(fd->file, 0, SEEK_END);
			put32(out, (uint32_t)ftell(fd->file));
			put32(out + 4, (uint32_t)pos);
			fseek(fd->file, pos, SEEK_SET);
			return kOk;
		}
		// the Portal of Power (portal.cpp)
		if (fd && fd->path == "/dev/usb/hid")
			return hidIoctl(req, ioctl, in, inLen, out, outLen);
		// STM's event hook is answered when an event happens (reset/power button): never, for now
		if (fd && fd->path == "/dev/stm/eventhook" && ioctl == 0x1000)
		{
			log("ios: ioctl %s 0x%x: event hook registered (pending)", name, ioctl);
			return std::nullopt;
		}
		// not handled: success with a zeroed output (left as it was, the game read leftovers as pointers)
		if (out && outLen)
			memset(memory_base + out, 0, outLen);
		static int s_unhandled = 0;
		if (s_unhandled++ < 200)
			log("ios: ioctl %s 0x%x in %08x/%u out %08x/%u (not handled: 0, output zeroed)", name, ioctl, in, inLen, out, outLen);
		return kOk;
	}
	case 7: // ioctlv
	{
		const uint32_t ioctl = be32(req + 0xC), nIn = be32(req + 0x10), nIo = be32(req + 0x14), vec = be32(req + 0x18);
		auto vecAddr = [&](uint32_t i) { return be32(vec + i * 8); };
		auto vecLen = [&](uint32_t i) { return be32(vec + i * 8 + 4); };
		if (fd && fd->path == "/dev/fs")
			return fsIoctlv(ioctl, nIn, nIo, vec);
		// The Bluetooth dongle and the Wii Remote behind it (bt.cpp)
		if (fd && fd->path == "/dev/usb/oh1/57e/305")
			return btIoctlv(req, ioctl, nIn, nIo, vec);
		// DI OpenPartition: the TMD into the first output vector, ES's result (0) into the second
		if (fd && fd->path == "/dev/di" && ioctl == 0x8B && nIo >= 2)
		{
			const std::vector<uint8_t> tmd = discTmd();
			memset(memory_base + vecAddr(nIn), 0, vecLen(nIn));
			memcpy(memory_base + vecAddr(nIn), tmd.data(), std::min<size_t>(tmd.size(), vecLen(nIn)));
			put32(vecAddr(nIn + 1), 0);
			s_partitionOpen = true;
			log("ios: di OpenPartition at %llx", (unsigned long long)be32(vecAddr(0) + 4) << 2);
			return 1;
		}
		// ES: this title (the disc's game, 00010000-53535045 "SSPE")
		if (fd && fd->path == "/dev/es")
		{
			constexpr uint64_t kTitle = 0x0001000053535045ull;
			if (ioctl == 0x20 && nIo >= 1 && vecLen(nIn) >= 8) // GetTitleID
			{
				put32(vecAddr(nIn), (uint32_t)(kTitle >> 32));
				put32(vecAddr(nIn) + 4, (uint32_t)kTitle);
				log("ios: es GetTitleID -> %016llx", (unsigned long long)kTitle);
				return kOk;
			}
			if (ioctl == 0x1B && nIo >= 1 && vecLen(nIn) >= 0xD8) // DIGetTicketView (no ticket given: the disc's)
			{
				const std::vector<uint8_t> ticket = discTicket();
				uint8_t* view = memory_base + vecAddr(nIn);
				memset(view, 0, 0xD8);
				view[3] = ticket[0x1BC]; // (the version, as a u32)
				memcpy(view + 4, ticket.data() + 0x1D0, 0xD4);
				log("ios: es DIGetTicketView (the disc's ticket)");
				return kOk;
			}
			if (ioctl == 0x16 && nIo >= 2) // GetConsumption: no limits
			{
				put32(vecAddr(nIn + 1), 0);
				log("ios: es GetConsumption -> 0 limits");
				return kOk;
			}
			if (ioctl == 0x1D && nIn >= 1 && nIo >= 1) // GetTitleDir(title)
			{
				const uint64_t title = (uint64_t)be32(vecAddr(0)) << 32 | be32(vecAddr(0) + 4);
				char dir[32];
				snprintf(dir, sizeof(dir), "/title/%08x/%08x/data", (uint32_t)(title >> 32), (uint32_t)title);
				memset(memory_base + vecAddr(nIn), 0, vecLen(nIn));
				memcpy(memory_base + vecAddr(nIn), dir, strlen(dir) < vecLen(nIn) ? strlen(dir) : vecLen(nIn) - 1);
				log("ios: es GetTitleDir %016llx -> %s", (unsigned long long)title, dir);
				return kOk;
			}
		}
		// NCD: the network link. No network on this console (the game has no online mode).
		if (fd && fd->path == "/dev/net/ncd/manage" && ioctl == 0x7 && nIo >= 1 && vecLen(nIn) >= 8)
		{
			memset(memory_base + vecAddr(nIn), 0, vecLen(nIn));
			put32(vecAddr(nIn) + 4, 2); // LINK_NONE
			return kOk;
		}
		char vecs[256] = {};
		int at = 0;
		for (uint32_t i = 0; i < nIn + nIo && i < 6 && at < 200; i++)
			at += snprintf(vecs + at, sizeof(vecs) - at, " %s%08x/%u", i < nIn ? "in " : "io ", be32(vec + i * 8), be32(vec + i * 8 + 4));
		// not handled: success with the outputs zeroed
		for (uint32_t i = nIn; i < nIn + nIo; i++)
			if (vecAddr(i) && vecLen(i))
				memset(memory_base + vecAddr(i), 0, vecLen(i));
		static int s_unhandled = 0;
		if (s_unhandled++ < 200)
			log("ios: ioctlv %s 0x%x%s (not handled: 0, outputs zeroed)", name, ioctl, vecs);
		return kOk;
	}
	default:
		log("ios: command %u on %u (%s) (not handled)", cmd, fdn, name);
		return kIpcEnoent;
	}
}

void reply(uint32_t req, int32_t result)
{
	const uint32_t cmd = be32(req);
	put32(req + 4, (uint32_t)result);
	put32(req + 8, cmd);
	put32(req, 8); // IPC_REPLY
	s_replies.push_back(req);
}

bool ready()
{
	return !s_ctrl.Y1 && !s_ctrl.Y2 && !(s_irqFlags & kIrqIpc);
}

// IOS takes the next request, or sends the next reply, when the PPC side is ready for it
void updateIpc()
{
	if (!ready())
		return;
	if (!s_requests.empty())
	{
		s_ctrl.X1 = false;
		s_ctrl.Y2 = true; // ack
		const uint32_t req = s_requests.front();
		s_requests.pop_front();
		if (auto result = iosRun(req))
			reply(req, *result);
		return;
	}
	if (!s_replies.empty())
	{
		s_armMsg = s_replies.front();
		s_replies.pop_front();
		s_ctrl.Y1 = true;
	}
}
} // namespace

// the IPC interrupt line into PI (cause 0x4000)
bool iosInterruptLine()
{
	if ((s_ctrl.Y1 && s_ctrl.IY1) || (s_ctrl.Y2 && s_ctrl.IY2))
		s_irqFlags |= kIrqIpc;
	return (s_irqFlags & s_irqMask) != 0;
}

bool iosOwns(uint32_t a)
{
	return a == 0xCD000000 || a == 0xCD000004 || a == 0xCD000008 || a == 0xCD000030 || a == 0xCD000034 || a == 0xCD000064;
}

uint32_t iosRead(uint32_t a)
{
	switch (a)
	{
	case 0xCD000000: return s_ppcMsg;
	case 0xCD000004: return s_ctrl.ppc();
	case 0xCD000008: return s_armMsg;
	case 0xCD000030: return s_irqFlags;
	case 0xCD000034: return s_irqMask;
	case 0xCD000064: return 0xFFFFFFFF; // AHBPROT: everything accessible
	}
	return 0;
}

void iosWrite(uint32_t a, uint32_t v)
{
	switch (a)
	{
	case 0xCD000000:
		s_ppcMsg = v;
		break;
	case 0xCD000004:
		s_ctrl.X1 = v & 1;
		s_ctrl.X2 = (v >> 3) & 1;
		if ((v >> 2) & 1)
			s_ctrl.Y1 = false;
		if ((v >> 1) & 1)
			s_ctrl.Y2 = false;
		s_ctrl.IY1 = (v >> 4) & 1;
		s_ctrl.IY2 = (v >> 5) & 1;
		// (the interrupt fires when Y1/Y2 is written with its enable set, even when that clears it)
		if (((v >> 2) & 1 && s_ctrl.IY1) || ((v >> 1) & 1 && s_ctrl.IY2))
			s_irqFlags |= kIrqIpc;
		if (s_ctrl.X1)
			s_requests.push_back(s_ppcMsg);
		updateIpc();
		break;
	case 0xCD000030:
		s_irqFlags &= ~v;
		updateIpc();
		break;
	case 0xCD000034:
		s_irqMask = v;
		// (Dolphin: writing the mask with the IPC bit resets IPC)
		if (v & kIrqIpc)
		{
			s_ctrl = Ctrl();
			s_ppcMsg = s_armMsg = 0;
		}
		updateIpc();
		break;
	}
}

// a request left pending (a device answers it later: the Bluetooth dongle's event and data buffers)
void iosReplyLater(uint32_t req, int32_t result)
{
	reply(req, result);
	updateIpc();
}

// called by the runtime between runs: replies queued while the PPC wasn't ready go out
void iosPoll()
{
	diPoll();
	updateIpc();
}
