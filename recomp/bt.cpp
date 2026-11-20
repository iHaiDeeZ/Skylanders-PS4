// Skylanders runtime: the Wii's Bluetooth dongle (/dev/usb/oh1/57e/305) and an emulated Wii Remote
// with a Nunchuk, all in software. Behaviour after Dolphin's BTEmu.cpp, WiimoteDevice.cpp,
// WiimoteHIDAttr.cpp and WiimoteEmu (GPL-2.0-or-later; this file is under the same terms).
//
// The game's own Bluetooth stack (BTE/WPAD) runs unchanged on top of it:
//   USB (IOS "V0" requests on the dongle):
//     ioctlv 0x0  control message: an HCI command (opcode, length, parameters; little-endian)
//     ioctlv 0x1  bulk message: endpoint 0x02 = ACL data to the remote, 0x82 = a buffer for ACL data
//     ioctlv 0x2  interrupt message: endpoint 0x81 = a buffer for an HCI event
//   Buffers given to fill stay pending until there is an event / ACL packet for them.
//   HCI: the controller's answers to the stack's commands, and a connection initiated by the remote
//     (Connection Request -> Accept -> Role Change + Connection Complete), handle 0x100 + remote.
//   L2CAP: the signalling channel; the remote opens HID control (PSM 0x11) and interrupt (PSM 0x13)
//     channels itself after the host accepted it, configures them (MTU 185), answers SDP (PSM 1).
//   The Wii Remote (HID on the interrupt channel, 0xA2 = output report, 0xA1 = input report):
//     0x11 LEDs, 0x12 reporting mode, 0x13/0x1A IR, 0x14 speaker, 0x15 status, 0x16 write memory,
//     0x17 read memory, 0x18 speaker data, 0x19 speaker mute; acks (0x22), status (0x20), read
//     replies (0x21), data reports 0x30-0x37/0x3D. EEPROM with Dolphin's IR and accel calibration;
//     a Nunchuk (ID 00 00 A4 20 00 00) in the extension registers at 0xA40000.
// Input comes from bt_input (neutral until the DS4 and the binding layer feed it).

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <deque>
#include <map>
#include <vector>
#include "wiimote_crypt.h" // (Dolphin's extension encryption)
#include "wiimap.h"        // (WiiState: what the binding layer makes of the DualShock 4)
#include <cmath>
#include <string>
#include <cstdlib>
#include <optional>
#include <array>

extern uint8_t* memory_base;
void iosReplyLater(uint32_t req, int32_t result); // ios.cpp: a pending request's reply

// What the emulated remote reports (the binding layer fills it; neutral by default).
struct BtInput
{
	uint16_t buttons = 0;              // Wii Remote core buttons (wiibrew bit layout: byte0 << 8 | byte1)
	uint8_t accel[3] = {0x80, 0x80, 0x9A}; // 8-bit X, Y, Z (flat, face up: Z = 1 g)
	bool nunchukC = false, nunchukZ = false;
	uint8_t stick[2] = {0x80, 0x80};
	uint8_t nunchukAccel[3] = {0x80, 0x80, 0xB3};
	bool pointer = false;              // the pointer on the screen: where (-1..1, x right, y up)
	float px = 0, py = 0;
} bt_input;

// the binding layer's state and the rumble (ps4/ps4_input.cpp sets them; the PC host has none: its
// input is SSA_PRESS)
bool (*g_btReadInput)(WiiState& out) = nullptr;
void (*g_btRumble)(bool on) = nullptr;

namespace
{
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

uint32_t be32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(memory_base + a)); }
uint16_t be16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(memory_base + a)); }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
void putLe16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

constexpr int kAclPktSize = 339, kAclPktNum = 10, kScoPktSize = 64, kScoPktNum = 0;
constexpr uint8_t kRemoteBd[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0x00}; // remote 0 (last byte: index)
constexpr uint8_t kControllerBd[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0xFF};
constexpr uint16_t kHandle = 0x100;                                       // 0x100 + remote index

// ---- USB endpoints: the buffers the stack gave us to fill ----
struct Pending
{
	uint32_t req = 0, data = 0, length = 0;
};
std::optional<Pending> s_eventBuffer, s_aclBuffer;
std::deque<std::vector<uint8_t>> s_events, s_acl;
uint8_t s_scanEnable = 0;
uint32_t s_completedPackets = 0;
int s_logBudget = 400;

void deliver()
{
	if (s_eventBuffer && !s_events.empty())
	{
		const std::vector<uint8_t>& e = s_events.front();
		const uint32_t n = std::min<uint32_t>((uint32_t)e.size(), s_eventBuffer->length);
		memcpy(memory_base + s_eventBuffer->data, e.data(), n);
		iosReplyLater(s_eventBuffer->req, (int32_t)n);
		s_eventBuffer.reset();
		s_events.pop_front();
	}
	// (ACL data only goes out once no events wait, as Dolphin does)
	if (s_aclBuffer && !s_acl.empty() && s_events.empty())
	{
		const std::vector<uint8_t>& p = s_acl.front();
		if (s_logBudget > 0)
		{
			s_logBudget--;
			log("bt: acl to host: %zu bytes, cid 0x%04x, first %02x %02x", p.size(), le16(&p[2]), p.size() > 4 ? p[4] : 0, p.size() > 5 ? p[5] : 0);
		}
		uint8_t* out = memory_base + s_aclBuffer->data;
		putLe16(out, kHandle | (0x2 << 12)); // packet start, point to point
		putLe16(out + 2, (uint16_t)p.size());
		memcpy(out + 4, p.data(), p.size());
		iosReplyLater(s_aclBuffer->req, (int32_t)(4 + p.size()));
		s_aclBuffer.reset();
		s_acl.pop_front();
	}
}

void event(std::vector<uint8_t> e)
{
	if (s_logBudget > 0)
	{
		s_logBudget--;
		log("bt: event 0x%02x (%zu bytes)", e[0], e.size());
	}
	s_events.push_back(std::move(e));
	deliver();
}

void commandComplete(uint16_t opcode, std::vector<uint8_t> params)
{
	std::vector<uint8_t> e = {0x0E, (uint8_t)(3 + params.size()), 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
	e.insert(e.end(), params.begin(), params.end());
	event(std::move(e));
}

void commandStatus(uint16_t opcode)
{
	event({0x0F, 4, 0x00, 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8)});
}

std::vector<uint8_t> bdBytes(const uint8_t* bd) { return std::vector<uint8_t>(bd, bd + 6); }

// an input data report (HID interrupt channel: 0xA1, report 0x30-0x3F)
bool isDataReport(const std::vector<uint8_t>& p)
{
	return p.size() > 5 && p[4] == 0xA1 && (p[5] & 0xF0) == 0x30;
}

uint64_t s_reportsMerged = 0;

void acl(std::vector<uint8_t> packet)
{
	// a data report while an older one still waits for the game to take it: the older one gets the
	// newer state. The remote reports at 200 Hz; a game running slower takes fewer, and queuing them
	// all made the controls lag behind more and more (seconds); the game now always reads the latest
	if (isDataReport(packet))
		for (std::vector<uint8_t>& q : s_acl)
			if (isDataReport(q) && q.size() == packet.size() && q[2] == packet[2] && q[3] == packet[3])
			{
				q = std::move(packet);
				s_reportsMerged++;
				deliver();
				return;
			}
	s_acl.push_back(std::move(packet));
	deliver();
}

// ---- the Wii Remote ----
enum class Baseband { RequestConnection, Inactive, Complete };
Baseband s_baseband = Baseband::RequestConnection;
bool s_hidLinking = false;

struct Channel
{
	uint16_t psm = 0, remoteCid = 0, remoteMtu = 0;
	enum State { Inactive, ConfigPending, Complete } state = Inactive;
	bool accepted() const { return remoteCid != 0; }
	bool complete() const { return accepted() && remoteMtu != 0 && state == Complete; }
};
std::map<uint16_t, Channel> s_channels; // by local CID
uint8_t s_ident = 0x80;

Channel* channelWithPsm(uint16_t psm, uint16_t* cid = nullptr)
{
	for (auto& [c, ch] : s_channels)
		if (ch.psm == psm)
		{
			if (cid)
				*cid = c;
			return &ch;
		}
	return nullptr;
}

uint16_t newCid()
{
	uint16_t cid = 0x40;
	while (s_channels.count(cid))
		cid++;
	return cid;
}

void signal(uint8_t ident, uint8_t code, const std::vector<uint8_t>& data)
{
	std::vector<uint8_t> p(8 + data.size());
	putLe16(&p[0], (uint16_t)(4 + data.size()));
	putLe16(&p[2], 0x0001); // signalling channel
	p[4] = code;
	p[5] = ident;
	putLe16(&p[6], (uint16_t)data.size());
	memcpy(&p[8], data.data(), data.size());
	acl(std::move(p));
}

void sendOnChannel(uint16_t remoteCid, const std::vector<uint8_t>& payload)
{
	std::vector<uint8_t> p(4 + payload.size());
	putLe16(&p[0], (uint16_t)payload.size());
	putLe16(&p[2], remoteCid);
	memcpy(&p[4], payload.data(), payload.size());
	acl(std::move(p));
}

// -- memory: EEPROM (0x0000-0x16FF) and registers (0xA2xxxx speaker, 0xA4xxxx extension, 0xB0xxxx IR)
std::array<uint8_t, 0x1700> s_eeprom{};
std::array<uint8_t, 0x100> s_extReg{}, s_speakerReg{}, s_irReg{};
uint8_t s_leds = 0, s_mode = 0x30;
bool s_continuous = false, s_irEnabled = false, s_speakerEnabled = false, s_speakerMuted = false;
// The extension's encryption (Dolphin's EncryptedExtension): on while its register 0xF0 holds 0xAA,
// with tables made from the key the game writes to 0x40-0x4F (made again after a write there). SSPE52
// turns it on after detecting the Nunchuk (0x55 to 0xF0, then 0xAA and the key): without it the game
// decrypted the plain stick, C/Z and calibration bytes into a stick held in one direction (v00.15:
// "whatever I press it goes to the same place").
WiimoteEmu::EncryptionKey s_extKey;
bool s_extKeyDirty = true;

void extEncrypt(uint8_t* data, uint32_t addr, uint32_t len)
{
	if (s_extReg[0xF0] != 0xAA)
		return;
	if (s_extKeyDirty)
	{
		WiimoteEmu::KeyGen::ExtKeyData key;
		memcpy(key.data(), &s_extReg[0x40], key.size());
		s_extKey = WiimoteEmu::KeyGen1stParty().GenerateFromExtensionKeyData(key);
		s_extKeyDirty = false;
		log("bt: extension encryption key %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", key[0], key[1], key[2],
			key[3], key[4], key[5], key[6], key[7], key[8], key[9], key[10], key[11], key[12], key[13], key[14], key[15]);
	}
	s_extKey.Encrypt(data, addr, len);
}

void checksum(uint8_t* data, int n, int cksumBytes)
{
	uint8_t sum = 0x55;
	for (int i = 0; i < n - cksumBytes; i++)
		sum += data[i];
	for (int i = n - cksumBytes; i < n; i++)
	{
		data[i] = sum;
		sum += 0x55;
	}
}

void initRemote()
{
	constexpr uint16_t lowX = 0x7F, lowY = 0x5D, highX = 0x380, highY = 0x2A2;
	uint8_t ir[11] = {lowX & 0xFF, lowY & 0xFF,
		(uint8_t)(((lowY & 0x300) >> 2) | ((lowX & 0x300) >> 4) | ((lowY & 0x300) >> 6) | ((highX & 0x300) >> 8)), highX & 0xFF, lowY & 0xFF,
		highX & 0xFF, highY & 0xFF, (uint8_t)(((highY & 0x300) >> 2) | ((highX & 0x300) >> 4) | ((highY & 0x300) >> 6) | ((lowX & 0x300) >> 8)),
		lowX & 0xFF, highY & 0xFF, 0};
	checksum(ir, 11, 1);
	memcpy(&s_eeprom[0x00], ir, 11);
	memcpy(&s_eeprom[0x0B], ir, 11);
	uint8_t accel[10] = {0x80, 0x80, 0x80, 0, 0x9A, 0x9A, 0x9A, 0, 0, 0};
	checksum(accel, 10, 1);
	memcpy(&s_eeprom[0x16], accel, 10);
	memcpy(&s_eeprom[0x20], accel, 10);
	static const uint8_t data16d0[24] = {0x00, 0x00, 0x00, 0xFF, 0x11, 0xEE, 0x00, 0x00, 0x33, 0xCC, 0x44, 0xBB,
		0x00, 0x00, 0x66, 0x99, 0x77, 0x88, 0x00, 0x00, 0x2B, 0x01, 0xE8, 0x13};
	memcpy(&s_eeprom[0x16D0], data16d0, 24);
	// Nunchuk: calibration at 0x20 (and a copy at 0x30), ID at 0xFA
	uint8_t cal[16] = {0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00, 0x80 + 0x60, 0x80 - 0x60, 0x80, 0x80 + 0x60, 0x80 - 0x60, 0x80, 0, 0};
	checksum(cal, 16, 2);
	memcpy(&s_extReg[0x20], cal, 16);
	memcpy(&s_extReg[0x30], cal, 16);
	static const uint8_t id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};
	memcpy(&s_extReg[0xFA], id, 6);
}

uint8_t buttons0() { return (uint8_t)(bt_input.buttons >> 8); }
uint8_t buttons1() { return (uint8_t)bt_input.buttons; }

void inputReport(const std::vector<uint8_t>& report)
{
	uint16_t cid = 0;
	Channel* ch = channelWithPsm(0x13, &cid);
	if (!ch || !ch->complete())
		return;
	std::vector<uint8_t> payload = {0xA1};
	payload.insert(payload.end(), report.begin(), report.end());
	sendOnChannel(ch->remoteCid, payload);
}

void ack(uint8_t reportId, uint8_t error)
{
	inputReport({0x22, buttons0(), buttons1(), reportId, error});
}

void status()
{
	const uint8_t flags = (uint8_t)((s_leds << 4) | (s_irEnabled ? 0x08 : 0) | (s_speakerEnabled ? 0x04 : 0) | 0x02 /* extension */);
	inputReport({0x20, buttons0(), buttons1(), flags, 0, 0, 0xC0 /* battery */});
}

uint8_t* space(uint32_t addr, bool registers, uint32_t& limit)
{
	if (!registers)
	{
		limit = (uint32_t)s_eeprom.size();
		return addr < s_eeprom.size() ? &s_eeprom[addr] : nullptr;
	}
	limit = 0x100;
	switch ((addr >> 16) & 0xFE)
	{
	case 0xA2: return &s_speakerReg[addr & 0xFF];
	case 0xA4: return &s_extReg[addr & 0xFF];
	case 0xB0: return &s_irReg[addr & 0xFF];
	}
	return nullptr;
}

void writeMemory(const uint8_t* r)
{
	const bool registers = r[0] & 0x04;
	const uint32_t addr = (uint32_t)r[1] << 16 | r[2] << 8 | r[3];
	const uint8_t size = r[4];
	uint32_t limit = 0;
	uint8_t* at = space(registers ? addr : (addr & 0xFFFF), registers, limit);
	if (!at || size > 16)
	{
		ack(0x16, 0x07);
		return;
	}
	const uint32_t offset = registers ? (addr & 0xFF) : (addr & 0xFFFF);
	const uint32_t n = std::min<uint32_t>(size, limit - offset);
	memcpy(at, r + 5, n);
	if (registers && ((addr >> 16) & 0xFE) == 0xA4)
	{
		// (the key area written: new tables when next needed; 0xF0 turns the encryption on or off)
		if (offset + n > 0x40 && offset < 0x50)
			s_extKeyDirty = true;
		if (offset <= 0xF0 && offset + n > 0xF0)
			log("bt: extension register 0xF0 = %02x (data %s)", s_extReg[0xF0], s_extReg[0xF0] == 0xAA ? "encrypted" : "plain");
	}
	if (s_logBudget > 0)
	{
		s_logBudget--;
		log("bt: wiimote write %s %06x +%u", registers ? "reg" : "eeprom", addr, size);
	}
	ack(0x16, 0x00);
}

void readMemory(const uint8_t* r)
{
	const bool registers = r[0] & 0x04;
	const uint32_t addr = (uint32_t)r[1] << 16 | r[2] << 8 | r[3];
	uint32_t size = (uint32_t)r[4] << 8 | r[5];
	if (s_logBudget > 0)
	{
		s_logBudget--;
		log("bt: wiimote read %s %06x +%u", registers ? "reg" : "eeprom", addr, size);
	}
	uint32_t limit = 0;
	const uint32_t base = registers ? (addr & 0xFF) : (addr & 0xFFFF);
	uint8_t* at = space(registers ? addr : base, registers, limit);
	uint32_t offset = 0;
	while (size > 0)
	{
		const uint32_t n = std::min<uint32_t>(16, size);
		std::vector<uint8_t> rep = {0x21, buttons0(), buttons1(), (uint8_t)(((n - 1) << 4) | (at ? 0 : 8)),
			(uint8_t)((addr + offset) >> 8), (uint8_t)(addr + offset)};
		for (uint32_t i = 0; i < 16; i++)
			rep.push_back(at && i < n && base + offset + i < limit ? at[offset + i] : 0);
		// (the extension's registers read encrypted while its encryption is on)
		if (at && registers && ((addr >> 16) & 0xFE) == 0xA4)
			extEncrypt(&rep[6], base + offset, n);
		inputReport(rep);
		if (!at)
			break;
		size -= n;
		offset += n;
	}
}

// The IR camera's data (Dolphin's CameraLogic): the sensor bar's two LEDs (20 cm apart, ~2 m away) as
// the camera (1024x768, 42 degrees wide, 4:3) sees them with the remote turned towards the pointer's
// place (Dolphin's IR defaults: 25 degrees of yaw and 15 of pitch across the screen). Pointing right
// moves the dots left in the camera's picture, pointing up moves them down. Only while the camera is on
// (report 0x13) with object tracking (register 0x30 = 0x08); the format is register 0x33's mode
// (1 basic: 5 bytes a pair; 3 extended: 3 bytes a dot); else 0xFF (nothing seen).
void irBytes(uint8_t* out, size_t n)
{
	memset(out, 0xFF, n);
	if (!s_irEnabled || s_irReg[0x30] != 0x08 || !bt_input.pointer)
		return;
	constexpr float kPi = 3.14159265f, kTanX = 0.383864f /* tan(21 deg) */, kTanY = 0.290596f /* tan(21 deg) * 3/4 */;
	constexpr float kDistance = 2.0f, kHalfBar = 0.1f;
	const float yaw = bt_input.px * 12.5f * kPi / 180.0f, pitch = bt_input.py * 7.5f * kPi / 180.0f;
	int x[2], y[2];
	for (int i = 0; i < 2; i++)
	{
		const float ax = std::atan((i ? kHalfBar : -kHalfBar) / kDistance) - yaw, ay = -pitch;
		x[i] = (int)std::lround(512.0f + 512.0f * std::tan(ax) / kTanX);
		y[i] = (int)std::lround(384.0f - 384.0f * std::tan(ay) / kTanY);
	}
	auto seen = [&](int i) { return x[i] >= 0 && x[i] < 1024 && y[i] >= 0 && y[i] < 768; };
	uint8_t data[12];
	memset(data, 0xFF, sizeof(data));
	const uint8_t mode = s_irReg[0x33];
	if (mode == 1)
	{
		// basic: x1, y1, (y1hi << 6 | x1hi << 4 | y2hi << 2 | x2hi), x2, y2 (an unseen dot: all ones)
		const int x1 = seen(0) ? x[0] : 0x3FF, y1 = seen(0) ? y[0] : 0x3FF, x2 = seen(1) ? x[1] : 0x3FF, y2 = seen(1) ? y[1] : 0x3FF;
		data[0] = (uint8_t)x1;
		data[1] = (uint8_t)y1;
		data[2] = (uint8_t)((y1 >> 8) << 6 | (x1 >> 8) << 4 | (y2 >> 8) << 2 | (x2 >> 8));
		data[3] = (uint8_t)x2;
		data[4] = (uint8_t)y2;
	}
	else if (mode == 3)
	{
		// extended: x, y, (yhi << 6 | xhi << 4 | size) per dot
		for (int i = 0; i < 2; i++)
			if (seen(i))
			{
				data[i * 3] = (uint8_t)x[i];
				data[i * 3 + 1] = (uint8_t)y[i];
				data[i * 3 + 2] = (uint8_t)((y[i] >> 8) << 6 | (x[i] >> 8) << 4 | 2);
			}
	}
	else
		return; // (full mode, two reports a frame: not done; no mode: 0xFF, as Dolphin)
	memcpy(out, data, std::min(n, sizeof(data)));
}

std::vector<uint8_t> extensionBytes(size_t n)
{
	std::vector<uint8_t> e(n, 0);
	const uint8_t ax = bt_input.nunchukAccel[0], ay = bt_input.nunchukAccel[1], az = bt_input.nunchukAccel[2];
	const uint8_t data[6] = {bt_input.stick[0], bt_input.stick[1], ax, ay, az,
		(uint8_t)((bt_input.nunchukC ? 0 : 0x02) | (bt_input.nunchukZ ? 0 : 0x01))};
	memcpy(e.data(), data, std::min<size_t>(n, 6));
	if (n > 6)
		memcpy(e.data() + 6, &s_extReg[6], std::min<size_t>(n - 6, 15));
	// (the report's bytes are the extension's registers from 0x00: encrypted with those addresses)
	extEncrypt(e.data(), 0, (uint32_t)n);
	return e;
}

void dataReport()
{
	std::vector<uint8_t> r = {s_mode};
	auto accel = [&] { r.insert(r.end(), bt_input.accel, bt_input.accel + 3); };
	auto ir = [&](size_t n) {
		uint8_t b[12];
		irBytes(b, n);
		r.insert(r.end(), b, b + n);
	};
	auto ext = [&](size_t n) { auto e = extensionBytes(n); r.insert(r.end(), e.begin(), e.end()); };
	if (s_mode != 0x3D)
	{
		r.push_back(buttons0());
		r.push_back(buttons1());
	}
	switch (s_mode)
	{
	case 0x30: break;
	case 0x31: accel(); break;
	case 0x32: ext(8); break;
	case 0x33: accel(); ir(12); break;
	case 0x34: ext(19); break;
	case 0x35: accel(); ext(16); break;
	case 0x36: ir(10); ext(9); break;
	case 0x37: accel(); ir(10); ext(6); break;
	case 0x3D: ext(21); break;
	default: return;
	}
	inputReport(r);
}

void outputReport(const uint8_t* r, uint32_t size)
{
	if (size < 1)
		return;
	const uint8_t id = r[0];
	const uint8_t* d = r + 1;
	const bool wantAck = size >= 2 && (d[0] & 0x02);
	// (every output report's first byte carries the rumble motor's bit)
	if (size >= 2 && g_btRumble)
	{
		static bool s_rumble = false;
		if ((d[0] & 1) != s_rumble)
		{
			s_rumble = d[0] & 1;
			g_btRumble(s_rumble);
		}
	}
	switch (id)
	{
	case 0x10: break; // rumble
	case 0x11:
		s_leds = d[0] >> 4;
		if (wantAck) ack(id, 0);
		break;
	case 0x12:
		s_continuous = d[0] & 0x04;
		s_mode = d[1];
		if (s_logBudget > 0)
			log("bt: wiimote reporting mode 0x%02x%s", s_mode, s_continuous ? " continuous" : "");
		if (wantAck) ack(id, 0);
		dataReport();
		break;
	case 0x13:
	case 0x1A:
		s_irEnabled = d[0] & 0x04;
		if (wantAck) ack(id, 0);
		break;
	case 0x14:
		s_speakerEnabled = d[0] & 0x04;
		if (wantAck) ack(id, 0);
		break;
	case 0x19:
		s_speakerMuted = d[0] & 0x04;
		if (wantAck) ack(id, 0);
		break;
	case 0x15: status(); break;
	case 0x16: writeMemory(d); break;
	case 0x17: readMemory(d); break;
	case 0x18: break; // speaker data (audio: later)
	default:
		if (s_logBudget > 0)
			log("bt: wiimote output report 0x%02x (not handled)", id);
		break;
	}
}

// -- L2CAP signalling, SDP and HID from the host
void l2capFromHost(const uint8_t* p, uint32_t size)
{
	if (size < 4)
		return;
	const uint16_t length = le16(p), cid = le16(p + 2);
	const uint8_t* data = p + 4;
	if (length != size - 4)
		return;
	if (cid == 0x0001) // signalling
	{
		uint32_t at = 0;
		while (at + 4 <= length)
		{
			const uint8_t code = data[at], ident = data[at + 1];
			const uint16_t len = le16(data + at + 2);
			const uint8_t* c = data + at + 4;
			switch (code)
			{
			case 0x02: // connection request: psm, scid
			{
				const uint16_t psm = le16(c), scid = le16(c + 2);
				std::vector<uint8_t> rsp(8, 0);
				putLe16(&rsp[2], scid);
				if (channelWithPsm(psm))
					putLe16(&rsp[4], 0x0004); // no resources (a second channel with the PSM)
				else
				{
					const uint16_t local = newCid();
					s_channels[local].psm = psm;
					s_channels[local].remoteCid = scid;
					putLe16(&rsp[0], local);
				}
				log("bt: l2cap connection request psm 0x%x", psm);
				signal(ident, 0x03, rsp);
				break;
			}
			case 0x03: // connection response: dcid, scid, result, status
			{
				const uint16_t dcid = le16(c), scid = le16(c + 2);
				if (s_channels.count(scid))
					s_channels[scid].remoteCid = dcid;
				log("bt: l2cap connection response for cid 0x%x: result %u", scid, le16(c + 4));
				break;
			}
			case 0x04: // configuration request: dcid, flags, options
			{
				const uint16_t dcid = le16(c);
				Channel& ch = s_channels[dcid];
				uint16_t mtu = 672;
				std::vector<uint8_t> rsp(6, 0);
				putLe16(&rsp[0], ch.remoteCid);
				for (uint32_t o = 4; o + 2 <= len;)
				{
					const uint8_t type = c[o], olen = c[o + 1];
					if (type == 0x01 && olen == 2)
						mtu = le16(c + o + 2);
					rsp.insert(rsp.end(), c + o, c + o + 2 + olen);
					o += 2 + olen;
				}
				ch.remoteMtu = mtu;
				signal(ident, 0x05, rsp);
				break;
			}
			case 0x05: // configuration response: scid, flags, result
			{
				const uint16_t scid = le16(c);
				if (s_channels.count(scid))
					s_channels[scid].state = Channel::Complete;
				log("bt: l2cap channel 0x%x configured (psm 0x%x)", scid, s_channels.count(scid) ? s_channels[scid].psm : 0);
				break;
			}
			case 0x06: // disconnection request: dcid, scid
			{
				const uint16_t dcid = le16(c), scid = le16(c + 2);
				s_channels.erase(dcid);
				std::vector<uint8_t> rsp(4);
				putLe16(&rsp[0], dcid);
				putLe16(&rsp[2], scid);
				signal(ident, 0x07, rsp);
				break;
			}
			default:
				log("bt: l2cap signal 0x%02x (not handled)", code);
				break;
			}
			at += 4 + len;
		}
		return;
	}
	auto it = s_channels.find(cid);
	if (it == s_channels.end())
		return;
	Channel& ch = it->second;
	if (ch.psm == 0x0001) // SDP
	{
		static const uint8_t packet1[] = {0x00, 0x7b, 0x00, 0x76, 0x36, 0x01, 0xcc, 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x01, 0x35,
			0x03, 0x19, 0x11, 0x24, 0x09, 0x00, 0x04, 0x35, 0x0d, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x11, 0x35, 0x03, 0x19, 0x00, 0x11, 0x09,
			0x00, 0x05, 0x35, 0x03, 0x19, 0x10, 0x02, 0x09, 0x00, 0x06, 0x35, 0x09, 0x09, 0x65, 0x6e, 0x09, 0x00, 0x6a, 0x09, 0x01, 0x00, 0x09, 0x00,
			0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x24, 0x09, 0x01, 0x00, 0x09, 0x00, 0x0d, 0x35, 0x0f, 0x35, 0x0d, 0x35, 0x06, 0x19, 0x01, 0x00,
			0x09, 0x00, 0x13, 0x35, 0x03, 0x19, 0x00, 0x11, 0x09, 0x01, 0x00, 0x25, 0x13, 0x4e, 0x69, 0x6e, 0x74, 0x65, 0x6e, 0x64, 0x6f, 0x20, 0x52,
			0x56, 0x4c, 0x2d, 0x43, 0x4e, 0x54, 0x2d, 0x30, 0x31, 0x09, 0x01, 0x02, 0x00, 0x76};
		static const uint8_t packet2[] = {0x00, 0x7b, 0x00, 0x76, 0x01, 0x25, 0x13, 0x4e, 0x69, 0x6e, 0x74, 0x65, 0x6e, 0x64, 0x6f, 0x20, 0x52, 0x56, 0x4c,
			0x2d, 0x43, 0x4e, 0x54, 0x2d, 0x30, 0x31, 0x09, 0x01, 0x02, 0x25, 0x08, 0x4e, 0x69, 0x6e, 0x74, 0x65, 0x6e, 0x64, 0x6f, 0x09, 0x02, 0x00,
			0x09, 0x01, 0x00, 0x09, 0x02, 0x01, 0x09, 0x01, 0x11, 0x09, 0x02, 0x02, 0x08, 0x04, 0x09, 0x02, 0x03, 0x08, 0x33, 0x09, 0x02, 0x04, 0x28,
			0x00, 0x09, 0x02, 0x05, 0x28, 0x01, 0x09, 0x02, 0x06, 0x35, 0xdf, 0x35, 0xdd, 0x08, 0x22, 0x25, 0xd9, 0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
			0x85, 0x10, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x06, 0x00, 0xff, 0x09, 0x01, 0x91, 0x00, 0x85, 0x11, 0x95, 0x01, 0x09,
			0x01, 0x91, 0x00, 0x85, 0x12, 0x95, 0x02, 0x09, 0x01, 0x91, 0x00, 0x02, 0x00, 0xec};
		static const uint8_t packet3[] = {0x00, 0x7b, 0x00, 0x76, 0x85, 0x13, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x14, 0x95, 0x01, 0x09, 0x01, 0x91,
			0x00, 0x85, 0x15, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x16, 0x95, 0x15, 0x09, 0x01, 0x91, 0x00, 0x85, 0x17, 0x95, 0x06, 0x09, 0x01,
			0x91, 0x00, 0x85, 0x18, 0x95, 0x15, 0x09, 0x01, 0x91, 0x00, 0x85, 0x19, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x1a, 0x95, 0x01, 0x09,
			0x01, 0x91, 0x00, 0x85, 0x20, 0x95, 0x06, 0x09, 0x01, 0x81, 0x00, 0x85, 0x21, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x22, 0x95, 0x04,
			0x09, 0x01, 0x81, 0x00, 0x85, 0x30, 0x95, 0x02, 0x09, 0x01, 0x81, 0x00, 0x85, 0x31, 0x95, 0x05, 0x09, 0x01, 0x81, 0x00, 0x85, 0x32, 0x95,
			0x0a, 0x09, 0x01, 0x81, 0x00, 0x85, 0x33, 0x95, 0x11, 0x09, 0x01, 0x02, 0x01, 0x62};
		static const uint8_t packet4[] = {0x00, 0x70, 0x00, 0x6d, 0x81, 0x00, 0x85, 0x34, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x35, 0x95, 0x15, 0x09,
			0x01, 0x81, 0x00, 0x85, 0x36, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x37, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x3d, 0x95, 0x15,
			0x09, 0x01, 0x81, 0x00, 0x85, 0x3e, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x3f, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0xc0, 0x09, 0x02,
			0x07, 0x35, 0x08, 0x35, 0x06, 0x09, 0x04, 0x09, 0x09, 0x01, 0x00, 0x09, 0x02, 0x08, 0x28, 0x00, 0x09, 0x02, 0x09, 0x28, 0x01, 0x09, 0x02,
			0x0a, 0x28, 0x01, 0x09, 0x02, 0x0b, 0x09, 0x01, 0x00, 0x09, 0x02, 0x0c, 0x09, 0x0c, 0x80, 0x09, 0x02, 0x0d, 0x28, 0x00, 0x09, 0x02, 0x0e,
			0x28, 0x00, 0x00};
		const uint8_t pdu = data[0];
		const uint16_t transaction = (uint16_t)(data[1] << 8 | data[2]);
		std::vector<uint8_t> out;
		if (pdu == 0x02) // service search: one HID record, handle 0x10000
			out = {0x03, (uint8_t)(transaction >> 8), (uint8_t)transaction, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};
		else if (pdu == 0x04) // service attribute: the record in parts, by continuation state
		{
			// (pdu, transaction, length, handle(4), max bytes(2), attribute list (seq: 35 len ...), continuation)
			const uint8_t listLen = data[12];
			const uint8_t* cont = data + 13 + listLen;
			const uint32_t state = cont[0] == 0x02 ? (uint32_t)(cont[1] << 8 | cont[2]) : 0;
			const uint8_t* part = state == 0 ? packet1 : state == 0x76 ? packet2 : state == 0xec ? packet3 : packet4;
			const size_t partSize = state == 0 ? sizeof(packet1) : state == 0x76 ? sizeof(packet2) : state == 0xec ? sizeof(packet3) : sizeof(packet4);
			out = {0x05, (uint8_t)(transaction >> 8), (uint8_t)transaction};
			out.insert(out.end(), part, part + partSize);
		}
		else
			log("bt: sdp pdu 0x%02x (not handled)", pdu);
		if (!out.empty())
			sendOnChannel(ch.remoteCid, out);
		return;
	}
	if (ch.psm == 0x0011 && length >= 1 && data[0] == 0x52) // HID control: set report (output)
	{
		sendOnChannel(ch.remoteCid, {0x00}); // handshake: success
		outputReport(data + 1, length - 1);
		return;
	}
	if (ch.psm == 0x0013 && length >= 1 && data[0] == 0xA2) // HID interrupt: output report
		outputReport(data + 1, length - 1);
}

// -- HCI commands from the host
void hciCommand(const uint8_t* cmd, uint32_t length)
{
	const uint16_t opcode = le16(cmd);
	const uint8_t* p = cmd + 3;
	if (s_logBudget > 0)
	{
		s_logBudget--;
		log("bt: hci command 0x%04x", opcode);
	}
	const std::vector<uint8_t> ok = {0x00};
	switch (opcode)
	{
	case 0x0C03: commandComplete(opcode, ok); break; // reset
	case 0x1005: // read buffer size
		commandComplete(opcode, {0x00, (uint8_t)kAclPktSize, (uint8_t)(kAclPktSize >> 8), (uint8_t)kScoPktSize, (uint8_t)kAclPktNum, 0, (uint8_t)kScoPktNum, 0});
		break;
	case 0x1001: commandComplete(opcode, {0x00, 0x03, 0xA7, 0x40, 0x03, 0x0F, 0x00, 0x0E, 0x43}); break; // read local version
	case 0x1009: // read BD_ADDR
	{
		std::vector<uint8_t> r = ok;
		r.insert(r.end(), kControllerBd, kControllerBd + 6);
		commandComplete(opcode, r);
		break;
	}
	case 0x1003: commandComplete(opcode, {0x00, 0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80}); break; // read local features
	case 0x0C0D: // read stored link key: the remote's key (return link keys), then the count
	{
		std::vector<uint8_t> keys = {0x15, 1 + 22, 1};
		keys.insert(keys.end(), kRemoteBd, kRemoteBd + 6);
		keys.insert(keys.end(), 16, 0xA0);
		event(keys);
		commandComplete(opcode, {0x00, 0xFF, 0x00, 0x01, 0x00});
		break;
	}
	case 0x0C12: commandComplete(opcode, {0x00, 0x00, 0x00}); break; // delete stored link key
	case 0x0C1A: // write scan enable
		s_scanEnable = p[0];
		commandComplete(opcode, ok);
		break;
	case 0x0C24: case 0x0C13: case 0x0C0A: case 0x0C33: case 0x0C18: case 0x0C45: case 0x0C47: case 0x0C05: case 0x0C43:
	case 0xFC4C: case 0xFC4F: case 0x0402:
		commandComplete(opcode, ok);
		break;
	case 0x0C37: commandComplete(opcode, {0x00, p[0], p[1]}); break; // link supervision timeout
	case 0x040B: case 0x040C: // link key reply / negative reply
	{
		std::vector<uint8_t> r = ok;
		r.insert(r.end(), p, p + 6);
		commandComplete(opcode, r);
		break;
	}
	case 0x0401: // inquiry: the remote isn't discoverable once connected; report none
		commandStatus(opcode);
		event({0x01, 1, 0x00});
		break;
	case 0x0409: // accept connection (bdaddr, role)
	{
		commandStatus(opcode);
		const bool ours = memcmp(p, kRemoteBd, 6) == 0;
		if (ours)
		{
			if (p[6] == 0) // the host wants to be master: role change
			{
				std::vector<uint8_t> e = {0x12, 8, 0x00};
				e.insert(e.end(), p, p + 6);
				e.push_back(0x00);
				event(e);
			}
			s_baseband = Baseband::Complete;
			s_hidLinking = true; // the remote sought the connection: it opens the HID channels
		}
		std::vector<uint8_t> e = {0x03, 11, (uint8_t)(ours ? 0x00 : 0x08), (uint8_t)kHandle, (uint8_t)(kHandle >> 8)};
		e.insert(e.end(), p, p + 6);
		e.push_back(0x01); // ACL
		e.push_back(0x00); // no encryption
		event(e);
		log("bt: connection accepted: wii remote 1 connected");
		break;
	}
	case 0x0405: // create connection (the host pages the remote)
	{
		commandStatus(opcode);
		const bool ours = memcmp(p, kRemoteBd, 6) == 0;
		if (ours)
			s_baseband = Baseband::Complete;
		std::vector<uint8_t> e = {0x03, 11, (uint8_t)(ours ? 0x00 : 0x08), (uint8_t)kHandle, (uint8_t)(kHandle >> 8)};
		e.insert(e.end(), p, p + 6);
		e.push_back(0x01);
		e.push_back(0x00);
		event(e);
		break;
	}
	case 0x0406: // disconnect (handle, reason)
		commandStatus(opcode);
		event({0x05, 4, 0x00, p[0], p[1], p[2]});
		s_baseband = Baseband::Inactive;
		s_channels.clear();
		s_hidLinking = false;
		log("bt: the game disconnected the remote (reason 0x%02x)", p[2]);
		break;
	case 0x0411: // authentication requested
		commandStatus(opcode);
		event({0x06, 3, 0x00, p[0], p[1]});
		break;
	case 0x0419: // remote name request
	{
		commandStatus(opcode);
		std::vector<uint8_t> e = {0x07, 255, 0x00};
		e.insert(e.end(), p, p + 6);
		const char name[] = "Nintendo RVL-CNT-01";
		std::vector<uint8_t> n(248, 0);
		memcpy(n.data(), name, sizeof(name) - 1);
		e.insert(e.end(), n.begin(), n.end());
		event(e);
		break;
	}
	case 0x041B: // read remote features
		commandStatus(opcode);
		event({0x0B, 11, 0x00, p[0], p[1], 0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00});
		break;
	case 0x041D: // read remote version
		commandStatus(opcode);
		event({0x0C, 8, 0x00, p[0], p[1], 0x02, 0x0F, 0x00, 0x29, 0x02});
		break;
	case 0x041F: // read clock offset
		commandStatus(opcode);
		event({0x1C, 5, 0x00, p[0], p[1], 0x18, 0x38});
		break;
	case 0x040F: // change connection packet type
		commandStatus(opcode);
		event({0x1D, 5, 0x00, p[0], p[1], p[2], p[3]});
		break;
	case 0x0803: // sniff mode: mode change to sniff with the max interval
		commandStatus(opcode);
		event({0x14, 6, 0x00, p[0], p[1], 0x02, p[2], p[3]});
		break;
	case 0x080D: commandStatus(opcode); break; // write link policy
	default:
		log("bt: hci command 0x%04x (fake complete)", opcode);
		commandComplete(opcode, {});
		break;
	}
}
} // namespace

// a request on the dongle's fd: its result, or nothing (answered later through iosReplyLater)
std::optional<int32_t> btIoctlv(uint32_t req, uint32_t ioctl, uint32_t nIn, uint32_t nIo, uint32_t vec)
{
	static bool init = (initRemote(), true);
	(void)init;
	auto vecAddr = [&](uint32_t i) { return be32(vec + i * 8); };
	auto vecLen = [&](uint32_t i) { return be32(vec + i * 8 + 4); };
	if (ioctl == 0x0 && nIn >= 6 && nIo >= 1) // control message: an HCI command
	{
		const uint16_t wLength = be16(vecAddr(4));
		hciCommand(memory_base + vecAddr(nIn), wLength);
		return (int32_t)wLength;
	}
	if ((ioctl == 0x1 || ioctl == 0x2) && nIn >= 2 && nIo >= 1)
	{
		const uint8_t endpoint = memory_base[vecAddr(0)];
		const uint16_t length = be16(vecAddr(1));
		const uint32_t data = vecAddr(nIn);
		if (ioctl == 0x2 && endpoint == 0x81) // a buffer for an HCI event
		{
			s_eventBuffer = Pending{req, data, std::min<uint32_t>(length, vecLen(nIn))};
			deliver();
			return std::nullopt;
		}
		if (ioctl == 0x1 && endpoint == 0x82) // a buffer for ACL data
		{
			s_aclBuffer = Pending{req, data, std::min<uint32_t>(length, vecLen(nIn))};
			deliver();
			return std::nullopt;
		}
		if (ioctl == 0x1 && endpoint == 0x02) // ACL data to the remote
		{
			const uint8_t* p = memory_base + data;
			const uint16_t handle = le16(p) & 0x0FFF, aclLength = le16(p + 2);
			if (s_logBudget > 0)
			{
				s_logBudget--;
				log("bt: acl from host: handle 0x%03x, %u bytes, cid 0x%04x, first %02x %02x", handle, aclLength, le16(p + 6), p[8], p[9]);
			}
			if (handle == kHandle)
			{
				s_completedPackets++;
				l2capFromHost(p + 4, aclLength);
			}
			return 0;
		}
	}
	log("bt: ioctlv 0x%x (not handled)", ioctl);
	return 0;
}

// called by the runtime regularly: the remote's connection and channel set-up, completed-packet
// counts, and input reports (continuous mode) at 200 Hz
// SSA_PRESS="seconds:BUTTON,...": scripted presses (each held a quarter second) until the DS4 and
// the binding layer feed bt_input. Buttons: A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT C Z; and the
// Nunchuk stick pushed all the way for 2 seconds: SUP SDOWN SLEFT SRIGHT.
void scriptedInput(uint64_t timeBase)
{
	struct Press
	{
		double at;
		uint16_t buttons;
		bool c, z;
		int sx = 0, sy = 0; // (stick: -1, 0, +1)
	};
	static std::vector<Press> s_presses;
	static bool s_parsed = false;
	if (!s_parsed)
	{
		s_parsed = true;
		const char* e = getenv("SSA_PRESS");
		for (const char* p = e; p && *p;)
		{
			char* end = nullptr;
			const double at = strtod(p, &end);
			if (!end || *end != ':')
				break;
			p = end + 1;
			std::string name;
			while (*p && *p != ',')
				name += *p++;
			if (*p == ',')
				p++;
			static const struct { const char* name; uint16_t bits; } kButtons[] = {{"LEFT", 0x0100}, {"RIGHT", 0x0200},
				{"DOWN", 0x0400}, {"UP", 0x0800}, {"PLUS", 0x1000}, {"2", 0x0001}, {"1", 0x0002}, {"B", 0x0004}, {"A", 0x0008},
				{"MINUS", 0x0010}, {"HOME", 0x0080}};
			Press pr{at, 0, name == "C", name == "Z"};
			for (const auto& b : kButtons)
				if (name == b.name)
					pr.buttons = b.bits;
			pr.sx = name == "SLEFT" ? -1 : name == "SRIGHT" ? 1 : 0;
			pr.sy = name == "SDOWN" ? -1 : name == "SUP" ? 1 : 0;
			s_presses.push_back(pr);
			log("bt: scripted press %s at %.2f s", name.c_str(), at);
		}
	}
	if (s_presses.empty())
		return;
	const double now = (double)timeBase / 60750000.0;
	uint16_t buttons = 0;
	bool c = false, z = false;
	int sx = 0, sy = 0;
	for (const Press& pr : s_presses)
	{
		if (now >= pr.at && now < pr.at + 0.25)
		{
			buttons |= pr.buttons;
			c |= pr.c;
			z |= pr.z;
		}
		if ((pr.sx || pr.sy) && now >= pr.at && now < pr.at + 2.0)
			sx = pr.sx, sy = pr.sy;
	}
	bt_input.buttons = buttons;
	bt_input.nunchukC = c;
	bt_input.nunchukZ = z;
	// (the calibration's range: centre 0x80, 0x60 each way)
	bt_input.stick[0] = (uint8_t)(0x80 + sx * 0x60);
	bt_input.stick[1] = (uint8_t)(0x80 + sy * 0x60);
}

void btUpdate(uint64_t timeBase)
{
	// the DualShock 4 through the binding layer (at most every millisecond), unless presses are scripted
	static const bool scripted = getenv("SSA_PRESS") != nullptr;
	static uint64_t s_lastInput = 0;
	if (scripted || !g_btReadInput)
		scriptedInput(timeBase);
	else if (timeBase - s_lastInput >= 60750000 / 1000)
	{
		s_lastInput = timeBase;
		WiiState w;
		if (g_btReadInput(w))
		{
			bt_input.buttons = w.buttons;
			bt_input.nunchukC = w.c;
			bt_input.nunchukZ = w.z;
			memcpy(bt_input.stick, w.stick, 2);
			memcpy(bt_input.accel, w.accel, 3);
			memcpy(bt_input.nunchukAccel, w.nunchukAccel, 3);
			bt_input.pointer = w.pointer;
			bt_input.px = w.px;
			bt_input.py = w.py;
		}
	}
	// the remote asks to connect once the host lets it (page scan)
	if (s_baseband == Baseband::RequestConnection && (s_scanEnable & 0x02))
	{
		std::vector<uint8_t> e = {0x04, 10};
		e.insert(e.end(), kRemoteBd, kRemoteBd + 6);
		e.insert(e.end(), {0x00, 0x04, 0x48, 0x01}); // class of device, ACL
		event(e);
		s_baseband = Baseband::Inactive;
		log("bt: wii remote 1 asks to connect");
	}
	if (s_baseband == Baseband::Complete)
	{
		// newly accepted channels: configure them (a real remote asks for MTU 185)
		for (auto& [cid, ch] : s_channels)
			if (ch.accepted() && ch.state == Channel::Inactive)
			{
				ch.state = Channel::ConfigPending;
				std::vector<uint8_t> cfg(4 + 4, 0);
				putLe16(&cfg[0], ch.remoteCid);
				cfg[4] = 0x01;
				cfg[5] = 2;
				putLe16(&cfg[6], 185);
				signal(s_ident++, 0x04, cfg);
			}
		// the remote opens HID control, then interrupt
		if (s_hidLinking)
		{
			auto link = [&](uint16_t psm) {
				if (Channel* ch = channelWithPsm(psm))
					return ch->complete();
				const uint16_t cid = newCid();
				s_channels[cid].psm = psm;
				std::vector<uint8_t> req(4);
				putLe16(&req[0], psm);
				putLe16(&req[2], cid);
				signal(0x02, 0x02, req);
				log("bt: the remote opens psm 0x%x", psm);
				return false;
			};
			if (link(0x11) && link(0x13))
			{
				s_hidLinking = false;
				log("bt: hid channels open");
			}
		}
	}
	// the controller reports packets it has sent on
	if (s_completedPackets)
	{
		event({0x13, 5, 1, (uint8_t)kHandle, (uint8_t)(kHandle >> 8), (uint8_t)s_completedPackets, (uint8_t)(s_completedPackets >> 8)});
		s_completedPackets = 0;
	}
	// input reports at 200 Hz in continuous mode (Dolphin: Wiimote::UPDATE_FREQ)
	static uint64_t s_lastReport = 0;
	constexpr uint64_t kInterval = 60750000 / 200;
	if (s_continuous && timeBase - s_lastReport >= kInterval)
	{
		s_lastReport = timeBase;
		dataReport();
		// (every 10 s: how many reports the game took against the ones merged into a waiting one)
		static uint64_t s_sent = 0, s_lastLog = 0, s_lastMerged = 0;
		s_sent++;
		if (timeBase - s_lastLog >= 607500000ull)
		{
			if (s_lastLog)
				log("bt: 10 s: %llu input reports, %llu merged into one the game had not taken yet", (unsigned long long)s_sent,
					(unsigned long long)(s_reportsMerged - s_lastMerged));
			s_lastLog = timeBase;
			s_sent = 0;
			s_lastMerged = s_reportsMerged;
		}
	}
	deliver();
}
