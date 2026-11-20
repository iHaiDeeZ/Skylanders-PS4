// Skylanders runtime: the game disc, served from the extracted files (dolphin-tool extract: DATA/).
//
// Raw disc (unencrypted reads, /dev/di 0x8D): rebuilt from what the extraction kept:
//   0x00000000  disc header (sys/boot.bin's first 0x440 bytes, disc/header.bin's 0x100 on top)
//   0x00040000  partition table: one data partition (type 0) at kPartition
//   0x0004E000  region info (disc/region.bin)
//   kPartition  partition header: ticket (0x2A4), TMD size/offset, cert size/offset, H3 offset,
//               data offset/size (offsets >> 2), then TMD at +0x2C0, certs after it, H3 at +0x8000
// Partition data (decrypted, /dev/di 0x71 once the partition is open), by offset:
//   0           sys/boot.bin      0x440      sys/bi2.bin     0x2440     sys/apploader.img
//   DOL offset  sys/main.dol      FST offset sys/fst.bin     (both from boot.bin, << 2)
//   every file at its FST offset (<< 2), under files/
// Gaps read as zeros.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
constexpr uint64_t kPartition = 0x0F800000;
constexpr uint64_t kTmdOffset = 0x2C0, kH3Offset = 0x8000, kDataOffset = 0x20000;

struct Piece
{
	uint64_t offset, size;
	std::string path;
};
std::vector<Piece> s_pieces;          // partition data, by offset
std::vector<uint8_t> s_raw;           // raw disc areas below the partition's data (built in memory)
std::vector<std::pair<uint64_t, std::vector<uint8_t>>> s_rawAreas;
std::string s_root;
uint64_t s_dataSize = 0;

std::vector<uint8_t> readFile(const std::string& path)
{
	std::vector<uint8_t> d;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		fseek(f, 0, SEEK_END);
		d.resize(ftell(f));
		fseek(f, 0, SEEK_SET);
		if (fread(d.data(), 1, d.size(), f) != d.size())
			d.clear();
		fclose(f);
	}
	return d;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x)
{
	v[at] = x >> 24; v[at + 1] = x >> 16; v[at + 2] = x >> 8; v[at + 3] = x;
}

uint64_t fileSize(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	const uint64_t n = ftell(f);
	fclose(f);
	return n;
}

// the FST's files with their paths (directories walked as the entries nest them)
void walkFst(const std::vector<uint8_t>& fst)
{
	const uint32_t count = be32(&fst[8]);
	const uint8_t* names = &fst[count * 12];
	std::vector<std::pair<uint32_t, std::string>> dirs = {{count, ""}}; // (end index, path)
	for (uint32_t i = 1; i < count; i++)
	{
		while (dirs.size() > 1 && i >= dirs.back().first)
			dirs.pop_back();
		const uint8_t* e = &fst[i * 12];
		const bool dir = e[0] != 0;
		const std::string name = (const char*)names + (be32(e) & 0xFFFFFF);
		const std::string path = dirs.back().second + "/" + name;
		if (dir)
			dirs.push_back({be32(e + 8), path});
		else
			s_pieces.push_back({(uint64_t)be32(e + 4) << 2, be32(e + 8), s_root + "/files" + path});
	}
}

void readPieces(uint64_t offset, uint32_t length, uint8_t* out)
{
	memset(out, 0, length);
	auto it = std::upper_bound(s_pieces.begin(), s_pieces.end(), offset, [](uint64_t o, const Piece& p) { return o < p.offset; });
	if (it != s_pieces.begin())
		--it;
	for (; it != s_pieces.end() && it->offset < offset + length; ++it)
	{
		const uint64_t start = std::max(offset, it->offset), end = std::min(offset + length, it->offset + it->size);
		if (start >= end)
			continue;
		if (FILE* f = fopen(it->path.c_str(), "rb"))
		{
			fseek(f, (long)(start - it->offset), SEEK_SET);
			const size_t n = fread(out + (start - offset), 1, end - start, f);
			(void)n;
			fclose(f);
		}
	}
}
} // namespace

bool discInit(const std::string& dataDir)
{
	s_root = dataDir;
	const std::vector<uint8_t> boot = readFile(dataDir + "/sys/boot.bin"), header = readFile(dataDir + "/disc/header.bin"),
							   region = readFile(dataDir + "/disc/region.bin"), ticket = readFile(dataDir + "/ticket.bin"),
							   tmd = readFile(dataDir + "/tmd.bin"), cert = readFile(dataDir + "/cert.bin"), h3 = readFile(dataDir + "/h3.bin"),
							   fst = readFile(dataDir + "/sys/fst.bin");
	if (boot.size() < 0x440 || header.size() < 0x100 || region.size() < 0x20 || ticket.size() != 0x2A4 || tmd.empty() || cert.empty() ||
		fst.size() < 12)
	{
		fprintf(stderr, "disc: %s is not a complete dolphin-tool extraction\n", dataDir.c_str());
		return false;
	}
	// partition data
	const uint64_t dolOffset = (uint64_t)be32(&boot[0x420]) << 2, fstOffset = (uint64_t)be32(&boot[0x424]) << 2;
	s_pieces = {{0, 0x440, dataDir + "/sys/boot.bin"}, {0x440, 0x2000, dataDir + "/sys/bi2.bin"},
		{0x2440, fileSize(dataDir + "/sys/apploader.img"), dataDir + "/sys/apploader.img"},
		{dolOffset, fileSize(dataDir + "/sys/main.dol"), dataDir + "/sys/main.dol"}, {fstOffset, fst.size(), dataDir + "/sys/fst.bin"}};
	walkFst(fst);
	std::sort(s_pieces.begin(), s_pieces.end(), [](const Piece& a, const Piece& b) { return a.offset < b.offset; });
	for (const Piece& p : s_pieces)
		s_dataSize = std::max(s_dataSize, p.offset + p.size);
	// raw areas
	std::vector<uint8_t> head(boot.begin(), boot.begin() + 0x440);
	memcpy(head.data(), header.data(), 0x100);
	std::vector<uint8_t> table(0x28, 0);
	put32(table, 0, 1);                        // group 0: one partition
	put32(table, 4, (0x40000 + 0x20) >> 2);    // its table
	put32(table, 0x20, (uint32_t)(kPartition >> 2));
	put32(table, 0x24, 0);                     // type 0: data
	std::vector<uint8_t> part(kH3Offset + h3.size(), 0);
	memcpy(part.data(), ticket.data(), ticket.size());
	const uint64_t certOffset = (kTmdOffset + tmd.size() + 0x1F) & ~0x1Full;
	put32(part, 0x2A4, (uint32_t)tmd.size());
	put32(part, 0x2A8, (uint32_t)(kTmdOffset >> 2));
	put32(part, 0x2AC, (uint32_t)cert.size());
	put32(part, 0x2B0, (uint32_t)(certOffset >> 2));
	put32(part, 0x2B4, (uint32_t)(kH3Offset >> 2));
	put32(part, 0x2B8, (uint32_t)(kDataOffset >> 2));
	put32(part, 0x2BC, (uint32_t)(((s_dataSize + 0x7BFF) / 0x7C00 * 0x8000) >> 2)); // (encrypted size: 0x7C00 data per 0x8000)
	memcpy(part.data() + kTmdOffset, tmd.data(), tmd.size());
	memcpy(part.data() + certOffset, cert.data(), cert.size());
	memcpy(part.data() + kH3Offset, h3.data(), h3.size());
	s_rawAreas = {{0, head}, {0x40000, table}, {0x4E000, region}, {kPartition, part}};
	fprintf(stderr, "disc: %zu files, partition data %llu MiB (DOL at %llx, FST at %llx)\n", s_pieces.size(),
		(unsigned long long)(s_dataSize >> 20), (unsigned long long)dolOffset, (unsigned long long)fstOffset);
	return true;
}

// raw (unencrypted) disc bytes
void discReadRaw(uint64_t offset, uint32_t length, uint8_t* out)
{
	memset(out, 0, length);
	for (const auto& [at, bytes] : s_rawAreas)
	{
		const uint64_t start = std::max(offset, at), end = std::min(offset + length, at + bytes.size());
		if (start < end)
			memcpy(out + (start - offset), bytes.data() + (start - at), end - start);
	}
}

// decrypted partition data
void discReadPartition(uint64_t offset, uint32_t length, uint8_t* out)
{
	readPieces(offset, length, out);
}

// the partition's ticket (ES ticket views)
std::vector<uint8_t> discTicket()
{
	return readFile(s_root + "/ticket.bin");
}

// the TMD (for opening the partition)
std::vector<uint8_t> discTmd()
{
	return readFile(s_root + "/tmd.bin");
}
