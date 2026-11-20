// Skylanders runtime: the Portal of Power, emulated behind IOS's /dev/usb/hid (HID v4). After
// Dolphin's USB_HIDv4, USBV4, SkylanderUSB/SkylanderPortal and SkylanderFigure/SkylanderCrypto
// (GPL-2.0-or-later, like the rest of the runtime).
//
// The game opens /dev/usb/hid, checks its version (0x40001), asks for device changes (the portal's
// USB descriptors come back at once: VID 0x1430, PID 0x0150, HID class, interrupt endpoints 0x81 and
// 0x02) and talks to the portal with HID control messages: a command letter and its bytes ('R' ready,
// 'A' activate, 'S' status, 'Q' read a block, 'W' write a block, 'C'/'J'/'L' lights, 'M' audio
// firmware, 'V'). Answers come back on interrupt-in reads, after ~22 ms; with nothing queued, an
// interrupt read gets the portal's status (2 bits per figure: added/ready/removing) after ~2 ms.
//
// Figures are 1 KB dumps (.sky), the 64 blocks of the toy's tag. SSA_PORTAL="path[=id[:variant]],..."
// puts them on the portal at boot; a path that doesn't exist yet becomes a new figure of that
// character (a fresh tag: random NUID, the character id/variant with its CRC, the sector keys, as
// Dolphin's portal creates them). The game's writes go back to the file. (The quick menu will place
// and remove figures later; the PS4 build keeps them in /data.)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <array>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <vector>

extern uint8_t* memory_base;
void iosReplyLater(uint32_t req, int32_t result); // ios.cpp

namespace
{
constexpr int32_t kIpcEinval = -4, kIpcEnoent = -6;
constexpr uint64_t kTicksPerUs = 61; // (the 60.75 MHz time base, near enough)

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...)
{
	static int s_count = 0;
	if (s_count++ > 400 && !getenv("SSA_PORTAL_LOG"))
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

uint8_t* guest(uint32_t a)
{
	return memory_base + (0x80000000u | (a & 0x1FFFFFFF));
}
uint32_t rd32(uint32_t a)
{
	const uint8_t* p = guest(a);
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
uint16_t rd16(uint32_t a)
{
	const uint8_t* p = guest(a);
	return (uint16_t)(p[0] << 8 | p[1]);
}
void wr32(uint32_t a, uint32_t v)
{
	uint8_t* p = guest(a);
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

// ---- figures (SkylanderFigure / SkylanderCrypto) ----
uint16_t crc16(const uint8_t* data, size_t n)
{
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < n; i++)
	{
		crc ^= (uint16_t)(data[i] << 8);
		for (int j = 0; j < 8; j++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

uint64_t crc48(const uint8_t* data, size_t n)
{
	uint64_t crc = 2ULL * 2ULL * 3ULL * 1103ULL * 12868356821ULL;
	for (size_t i = 0; i < n; i++)
	{
		crc ^= (uint64_t)data[i] << 40;
		for (int j = 0; j < 8; j++)
			crc = (crc & (1ULL << 47)) ? (crc << 1) ^ 0x42f0e1eba9ea3693ULL : crc << 1;
	}
	return crc & 0x0000FFFFFFFFFFFFULL;
}

// sector key A: a constant for sector 0, else from the NUID and the sector (CRC-48, byte-swapped)
uint64_t keyA(uint8_t sector, const uint8_t* nuid)
{
	if (sector == 0)
		return 73ULL * 2017ULL * 560381651ULL;
	const uint8_t data[5] = {nuid[0], nuid[1], nuid[2], nuid[3], sector};
	return __builtin_bswap64(crc48(data, 5)) >> 16;
}

struct Figure
{
	std::string path;
	std::array<uint8_t, 1024> data{};
	uint8_t status = 0; // 0 removed, 1 ready, 2 removing, 3 added
	std::deque<uint8_t> queued;
	bool present = false;

	void save() const
	{
		if (FILE* f = fopen(path.c_str(), "wb"))
		{
			fwrite(data.data(), 1, data.size(), f);
			fclose(f);
		}
	}
};

// a new tag for character id/variant (SkylanderFigure::Create)
void createFigure(Figure& fig, uint16_t id, uint16_t variant)
{
	auto& d = fig.data;
	d.fill(0);
	// sector trailers' access bits (little-endian words 0x690F0F0F, then 0x69080F7F)
	const uint8_t first[4] = {0x0F, 0x0F, 0x0F, 0x69}, other[4] = {0x7F, 0x0F, 0x08, 0x69};
	memcpy(&d[0x36], first, 4);
	for (int s = 1; s < 16; s++)
		memcpy(&d[s * 0x40 + 0x36], other, 4);
	std::random_device rd;
	for (int i = 0; i < 4; i++)
		d[i] = (uint8_t)rd();
	d[4] = d[0] ^ d[1] ^ d[2] ^ d[3]; // BCC
	d[5] = 0x81;                      // ATQA
	d[6] = 0x01;
	d[7] = 0x0F; // SAK
	d[0x10] = (uint8_t)id;
	d[0x11] = (uint8_t)(id >> 8);
	d[0x1C] = (uint8_t)variant;
	d[0x1D] = (uint8_t)(variant >> 8);
	const uint16_t crc = crc16(d.data(), 0x1E);
	d[0x1E] = (uint8_t)crc;
	d[0x1F] = (uint8_t)(crc >> 8);
	for (int s = 0; s < 16; s++)
	{
		const uint64_t key = keyA((uint8_t)s, d.data());
		for (int j = 0; j < 6; j++)
			d[s * 0x40 + 0x30 + (5 - j)] = (uint8_t)(key >> (j * 8));
	}
}

// ---- the portal (SkylanderPortal) ----
constexpr int kMaxFigures = 16;
Figure s_figures[kMaxFigures];
bool s_activated = false;
uint8_t s_counter = 0;
std::deque<std::array<uint8_t, 64>> s_queries; // answers waiting for interrupt-in reads

void loadFigures()
{
	const char* e = getenv("SSA_PORTAL");
	if (!e)
		return;
	std::string all = e;
	int slot = 0;
	for (size_t at = 0; at < all.size() && slot < kMaxFigures;)
	{
		size_t comma = all.find(',', at);
		std::string item = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		at = comma == std::string::npos ? all.size() : comma + 1;
		std::string path = item;
		long id = -1, variant = 0;
		if (size_t eq = item.find('='); eq != std::string::npos)
		{
			path = item.substr(0, eq);
			char* end = nullptr;
			id = strtol(item.c_str() + eq + 1, &end, 0);
			if (end && *end == ':')
				variant = strtol(end + 1, nullptr, 0);
		}
		Figure& fig = s_figures[slot];
		fig.path = path;
		if (FILE* f = fopen(path.c_str(), "rb"))
		{
			const size_t n = fread(fig.data.data(), 1, fig.data.size(), f);
			fclose(f);
			if (n != fig.data.size())
			{
				log("portal: %s is %zu bytes, not 1024: left out", path.c_str(), n);
				continue;
			}
			log("portal: figure %s on slot %d (id %u, variant 0x%04x)", path.c_str(), slot, fig.data[0x10] | fig.data[0x11] << 8,
				fig.data[0x1C] | fig.data[0x1D] << 8);
		}
		else if (id >= 0)
		{
			createFigure(fig, (uint16_t)id, (uint16_t)variant);
			fig.save();
			log("portal: new figure %s on slot %d (id %ld, variant 0x%04lx)", path.c_str(), slot, id, variant);
		}
		else
		{
			log("portal: %s can't be read (give =id to create it)", path.c_str());
			continue;
		}
		fig.present = true;
		fig.status = 3;
		fig.queued = {3, 1};
		slot++;
	}
}

void activate()
{
	if (s_activated)
		return;
	for (Figure& f : s_figures)
		if (f.status & 1)
		{
			f.queued.push_back(3);
			f.queued.push_back(1);
		}
	s_activated = true;
}

std::array<uint8_t, 64> status()
{
	uint32_t bits = 0;
	for (int i = kMaxFigures - 1; i >= 0; i--)
	{
		Figure& f = s_figures[i];
		if (!f.queued.empty())
		{
			f.status = f.queued.front();
			f.queued.pop_front();
		}
		bits = bits << 2 | f.status;
	}
	std::array<uint8_t, 64> r{};
	r[0] = 'S';
	memcpy(&r[1], &bits, 4); // (little-endian, as the portal sends it)
	r[5] = s_counter++;
	r[6] = s_activated ? 1 : 0;
	return r;
}

void queryBlock(uint8_t num, uint8_t block, uint8_t* r)
{
	if (num >= kMaxFigures || block >= 64)
		return;
	r[0] = 'Q';
	r[2] = block;
	if (s_figures[num].status & 1)
	{
		r[1] = 0x10 | num;
		memcpy(r + 3, &s_figures[num].data[block * 16], 16);
	}
	else
		r[1] = 0x01;
}

void writeBlock(uint8_t num, uint8_t block, const uint8_t* in, uint8_t* r)
{
	if (num >= kMaxFigures || block >= 64)
		return;
	r[0] = 'W';
	r[2] = block;
	if (s_figures[num].status & 1)
	{
		r[1] = 0x10 | num;
		memcpy(&s_figures[num].data[block * 16], in, 16);
		s_figures[num].save();
	}
	else
		r[1] = 0x01;
}

// ---- HID v4 ----
struct Pending
{
	uint32_t req;
	uint64_t due;
	int32_t result;
	uint32_t dataAddr, length;
	std::array<uint8_t, 64> data;
	bool fill;
};
std::vector<Pending> s_pending;
uint32_t s_changeHook = 0; // a device-change request waiting for a change
uint32_t s_changeOut = 0;
bool s_changePending = true; // (the portal is there from the start)
uint64_t s_now = 0;

void schedule(uint32_t req, int32_t result, uint64_t us, uint32_t dataAddr, uint32_t length, const std::array<uint8_t, 64>* data)
{
	Pending p{req, s_now + us * kTicksPerUs, result, dataAddr, length, {}, data != nullptr};
	if (data)
		p.data = *data;
	s_pending.push_back(p);
}

// the device entry: size, IOS id, then the descriptors (device, config, interface, endpoints), each
// padded to 4 bytes, big-endian fields
void deviceEntry(std::vector<uint8_t>& e)
{
	auto pad = [&] {
		while (e.size() % 4)
			e.push_back(0);
	};
	auto u16be = [&](uint16_t v) {
		e.push_back((uint8_t)(v >> 8));
		e.push_back((uint8_t)v);
	};
	e.assign(8, 0);
	// device: bLength, bDescriptorType, bcdUSB, class, subclass, protocol, maxPacket0, idVendor,
	// idProduct, bcdDevice, iManufacturer, iProduct, iSerial, bNumConfigurations
	e.insert(e.end(), {0x12, 0x01});
	u16be(0x0200);
	e.insert(e.end(), {0x00, 0x00, 0x00, 0x40});
	u16be(0x1430);
	u16be(0x0150);
	u16be(0x0100);
	e.insert(e.end(), {0x01, 0x02, 0x00, 0x01});
	pad();
	// config: bLength, type, wTotalLength, bNumInterfaces, bConfigurationValue, iConfiguration,
	// bmAttributes, bMaxPower
	e.insert(e.end(), {0x09, 0x02});
	u16be(0x0029);
	e.insert(e.end(), {0x01, 0x01, 0x00, 0x80, 0xFA});
	pad();
	// interface: 2 endpoints, HID class
	e.insert(e.end(), {0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00});
	pad();
	// endpoints: 0x81 in and 0x02 out, interrupt, 64 bytes, interval 1
	e.insert(e.end(), {0x07, 0x05, 0x81, 0x03});
	u16be(0x0040);
	e.push_back(0x01);
	pad();
	e.insert(e.end(), {0x07, 0x05, 0x02, 0x03});
	u16be(0x0040);
	e.push_back(0x01);
	pad();
	const uint32_t size = (uint32_t)e.size();
	e[0] = (uint8_t)(size >> 24);
	e[1] = (uint8_t)(size >> 16);
	e[2] = (uint8_t)(size >> 8);
	e[3] = (uint8_t)size;
	// (IOS id 0 at 4-7)
}

void replyDeviceChange()
{
	std::vector<uint8_t> e;
	deviceEntry(e);
	memcpy(guest(s_changeOut), e.data(), e.size());
	wr32(s_changeOut + (uint32_t)e.size(), 0xFFFFFFFF);
	iosReplyLater(s_changeHook, 0);
	log("portal: device change: the portal (%zu bytes of descriptors)", e.size());
	s_changeHook = 0;
}

// a control message to the portal: the command letter and its bytes in the data buffer
std::optional<int32_t> control(uint32_t req, uint32_t in)
{
	const uint8_t type = guest(in)[20], request = guest(in)[21];
	const uint16_t length = rd16(in + 26);
	const uint32_t dataAddr = rd32(in + 28);
	if (type != 0x21)
		return kIpcEinval;
	std::array<uint8_t, 64> ctrl{};
	int32_t count = 0;
	if (request != 0x09)
	{
		if (request == 0x0A || request == 0x0B)
			count = 8;
	}
	else
	{
		if (!length)
			return kIpcEinval;
		const uint8_t* buf = guest(dataAddr);
		std::array<uint8_t, 64> answer{};
		switch (buf[0])
		{
		case 'A':
			if (length == 2)
			{
				ctrl = {buf[0], buf[1]};
				answer = {0x41, buf[1], 0xFF, 0x77};
				s_queries.push_back(answer);
				count = 10;
				activate();
				log("portal: activate %u", buf[1]);
			}
			break;
		case 'C':
			if (length == 4)
			{
				ctrl = {0x43, buf[1], buf[2], buf[3]};
				count = 12;
			}
			break;
		case 'J':
			if (length == 7)
			{
				for (int i = 0; i < 7; i++)
					ctrl[i] = buf[i];
				count = 15;
				answer = {buf[0]};
				s_queries.push_back(answer);
			}
			break;
		case 'L':
			if (length == 5)
			{
				for (int i = 0; i < 5; i++)
					ctrl[i] = buf[i];
				count = 13;
			}
			break;
		case 'M':
			if (length == 2)
			{
				ctrl = {buf[0], buf[1]};
				count = 10;
				answer = {buf[0], buf[1], 0x00, 0x19};
				s_queries.push_back(answer);
			}
			break;
		case 'Q':
			if (length == 3)
			{
				queryBlock(buf[1] & 0xF, buf[2], answer.data());
				s_queries.push_back(answer);
				ctrl = {buf[0], buf[1], buf[2]};
				count = 11;
			}
			break;
		case 'R':
			if (length == 2)
			{
				ctrl = {0x52, 0x00};
				answer = {0x52, 0x02, 0x1B};
				s_queries.push_back(answer);
				count = 10;
				log("portal: ready");
			}
			break;
		case 'S':
			if (length == 1)
			{
				ctrl = {buf[0]};
				count = 9;
			}
			break;
		case 'V':
			if (length == 4)
			{
				for (int i = 0; i < 4; i++)
					ctrl[i] = buf[i];
				count = 12;
			}
			break;
		case 'W':
			if (length == 19)
			{
				writeBlock(buf[1] & 0xF, buf[2], buf + 3, answer.data());
				s_queries.push_back(answer);
				for (int i = 0; i < 19; i++)
					ctrl[i] = buf[i];
				count = 27;
			}
			break;
		default:
			log("portal: unknown command '%c' (%u bytes)", buf[0], length);
			break;
		}
	}
	if (!count)
		return kIpcEinval;
	schedule(req, count, 100, dataAddr, length, &ctrl);
	return std::nullopt;
}

// an interrupt message: a queued answer (after ~22 ms) or the status (after ~2 ms); audio (more
// than 32 bytes, out) is echoed at once
std::optional<int32_t> interrupt(uint32_t req, uint32_t in)
{
	const uint32_t length = rd32(in + 24), dataAddr = rd32(in + 28);
	if (!length)
		return kIpcEinval;
	if (length > 32 && length <= 64)
	{
		schedule(req, (int32_t)length, 0, dataAddr, 0, nullptr);
		return std::nullopt;
	}
	std::array<uint8_t, 64> r;
	uint64_t us;
	if (!s_queries.empty())
	{
		r = s_queries.front();
		s_queries.pop_front();
		us = 22000;
	}
	else
	{
		r = status();
		us = 2000;
	}
	schedule(req, 32, us, dataAddr, std::min<uint32_t>(length, 32), &r);
	return std::nullopt;
}
} // namespace

// /dev/usb/hid ioctls (ios.cpp): an answer now, or nullopt when it comes later (portalUpdate)
std::optional<int32_t> hidIoctl(uint32_t req, uint32_t ioctl, uint32_t in, uint32_t inLen, uint32_t out, uint32_t outLen)
{
	static bool s_loaded = false;
	if (!s_loaded)
	{
		s_loaded = true;
		loadFigures();
	}
	switch (ioctl)
	{
	case 6: // GETVERSION
		return 0x40001;
	case 0: // GETDEVICECHANGE
		if (!out || outLen != 0x600)
			return kIpcEinval;
		s_changeHook = req;
		s_changeOut = out;
		if (s_changePending)
		{
			s_changePending = false;
			replyDeviceChange();
		}
		return std::nullopt;
	case 7: // SHUTDOWN
		if (s_changeHook)
		{
			wr32(s_changeOut, 0xFFFFFFFF);
			iosReplyLater(s_changeHook, -1);
			s_changeHook = 0;
		}
		log("portal: HID shutdown");
		return 0;
	case 1: // SET_SUSPEND
	case 8: // CANCELINTERRUPT
		return 0;
	case 2: // CTRLMSG
	case 3: // INTRMSG_IN
	case 4: // INTRMSG_OUT
	case 5: // GET_US_STRING
		if (!in || inLen != 32)
			return kIpcEinval;
		if ((int32_t)rd32(in + 16) != 0)
			return kIpcEnoent;
		if (ioctl == 2)
			return control(req, in);
		if (ioctl == 5)
			return kIpcEinval;
		return interrupt(req, in);
	default:
		log("portal: HID ioctl %u (not handled)", ioctl);
		return 0;
	}
}

// completions due by now (called between runs)
void portalUpdate(uint64_t timeBase)
{
	s_now = timeBase;
	for (size_t i = 0; i < s_pending.size();)
	{
		Pending& p = s_pending[i];
		if (p.due > timeBase)
		{
			i++;
			continue;
		}
		if (p.fill && p.length)
			memcpy(guest(p.dataAddr), p.data.data(), std::min<uint32_t>(p.length, 64));
		const uint32_t req = p.req;
		const int32_t result = p.result;
		s_pending.erase(s_pending.begin() + (long)i);
		iosReplyLater(req, result);
	}
}
