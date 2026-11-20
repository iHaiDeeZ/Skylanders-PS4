// The DualShock 4 (and a PS Move) as the Wii Remote + Nunchuk (wiimap.h).
#include "wiimap.h"

#include "settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string.h>
#include <string>

namespace
{
enum In : uint8_t
{
	IN_NONE,
	IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE, IN_L1, IN_R1, IN_L2, IN_R2, IN_L3, IN_R3, IN_OPTIONS, IN_TOUCHPAD,
	IN_PAD_N, IN_PAD_S, IN_PAD_W, IN_PAD_E,
	IN_LX_NEG, IN_LX_POS, IN_LY_NEG, IN_LY_POS, IN_RX_NEG, IN_RX_POS, IN_RY_NEG, IN_RY_POS,
	IN_LEFT_STICK, IN_RIGHT_STICK, IN_DPAD,
};

struct InName
{
	const char* name; // DolphinPS4's (the settings file's)
	const char* desc; // how the menu writes it when no icon is drawn
	In id;
	uint32_t bit;     // libScePad's button bit (0: an axis or a group)
};

// libScePad's bits (OpenOrbis's ORBIS_PAD_BUTTON_*)
constexpr uint32_t B_L3 = 0x2, B_R3 = 0x4, B_OPTIONS = 0x8, B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80, B_L2 = 0x100,
	B_R2 = 0x200, B_L1 = 0x400, B_R1 = 0x800, B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000,
	B_TOUCHPAD = 0x100000;

const InName kNames[] = {
	{"Cross", "Cross", IN_CROSS, B_CROSS},
	{"Circle", "Circle", IN_CIRCLE, B_CIRCLE},
	{"Square", "Square", IN_SQUARE, B_SQUARE},
	{"Triangle", "Triangle", IN_TRIANGLE, B_TRIANGLE},
	{"L1", "L1", IN_L1, B_L1},
	{"R1", "R1", IN_R1, B_R1},
	{"L2", "L2", IN_L2, B_L2},
	{"R2", "R2", IN_R2, B_R2},
	{"L3", "L3", IN_L3, B_L3},
	{"R3", "R3", IN_R3, B_R3},
	{"Options", "Options", IN_OPTIONS, B_OPTIONS},
	{"Touchpad", "Touchpad", IN_TOUCHPAD, B_TOUCHPAD},
	{"Pad N", "D-Pad Up", IN_PAD_N, B_UP},
	{"Pad S", "D-Pad Down", IN_PAD_S, B_DOWN},
	{"Pad W", "D-Pad Left", IN_PAD_W, B_LEFT},
	{"Pad E", "D-Pad Right", IN_PAD_E, B_RIGHT},
	{"Left X-", "Left Stick Left", IN_LX_NEG, 0},
	{"Left X+", "Left Stick Right", IN_LX_POS, 0},
	{"Left Y-", "Left Stick Up", IN_LY_NEG, 0},
	{"Left Y+", "Left Stick Down", IN_LY_POS, 0},
	{"Right X-", "Right Stick Left", IN_RX_NEG, 0},
	{"Right X+", "Right Stick Right", IN_RX_POS, 0},
	{"Right Y-", "Right Stick Up", IN_RY_NEG, 0},
	{"Right Y+", "Right Stick Down", IN_RY_POS, 0},
	{"Left Stick", "Left Stick", IN_LEFT_STICK, 0},
	{"Right Stick", "Right Stick", IN_RIGHT_STICK, 0},
	{"D-Pad", "D-Pad", IN_DPAD, 0},
};

In lookup(const std::string& name)
{
	for (const InName& n : kNames)
		if (name == n.name)
			return n.id;
	return IN_NONE;
}

float axis(uint8_t v)
{
	return ((float)v - 127.5f) / 127.5f;
}

bool on(const PadState& p, In id)
{
	switch (id)
	{
	case IN_NONE: return false;
	case IN_L2: return (p.buttons & B_L2) || p.l2 > 128;
	case IN_R2: return (p.buttons & B_R2) || p.r2 > 128;
	case IN_LX_NEG: return p.lx < 64;
	case IN_LX_POS: return p.lx > 191;
	case IN_LY_NEG: return p.ly < 64;
	case IN_LY_POS: return p.ly > 191;
	case IN_RX_NEG: return p.rx < 64;
	case IN_RX_POS: return p.rx > 191;
	case IN_RY_NEG: return p.ry < 64;
	case IN_RY_POS: return p.ry > 191;
	case IN_LEFT_STICK:
	case IN_RIGHT_STICK:
	case IN_DPAD: return false;
	default:
		for (const InName& n : kNames)
			if (n.id == id)
				return (p.buttons & n.bit) != 0;
		return false;
	}
}

// a stick group as x right, y up (-1..1)
void group(const PadState& p, In id, float& x, float& y)
{
	x = y = 0;
	if (id == IN_LEFT_STICK)
	{
		x = axis(p.lx);
		y = -axis(p.ly);
	}
	else if (id == IN_RIGHT_STICK)
	{
		x = axis(p.rx);
		y = -axis(p.ry);
	}
	else if (id == IN_DPAD)
	{
		x = ((p.buttons & B_RIGHT) ? 1.0f : 0.0f) - ((p.buttons & B_LEFT) ? 1.0f : 0.0f);
		y = ((p.buttons & B_UP) ? 1.0f : 0.0f) - ((p.buttons & B_DOWN) ? 1.0f : 0.0f);
	}
}

// the bindings as input ids, rebuilt when they change
struct Table
{
	In a, b, one, two, minus, plus, home, shake, pointer, recenter, up, down, left, right, c, z, stick, nshake;
};
Table s_table;
uint32_t s_tableVersion = 0;

void rebuild()
{
	auto get = [](const char* key) { return lookup(ssa::binding(key)); };
	s_table = {get("A"), get("B"), get("1"), get("2"), get("-"), get("+"), get("Home"), get("Shake"), get("Pointer"), get("Recenter"),
		get("Up"), get("Down"), get("Left"), get("Right"), get("C"), get("Z"), get("Stick"), get("NShake")};
}

// the pointer's place (-1..1), moved by a stick or a gyro, kept between updates
float s_px = 0, s_py = 0;
double s_last = 0;
std::atomic<bool> s_recenter{false};

uint8_t accel8(float g, int zero, int perG)
{
	const int v = zero + (int)std::lround(g * (float)perG);
	return (uint8_t)std::clamp(v, 0, 255);
}
} // namespace

bool wiimapInputOn(const PadState& pad, const char* input)
{
	return on(pad, lookup(input));
}

void wiimapRecenter()
{
	s_recenter = true;
}

void wiimapUpdate(const PadState& pad, WiiState& out)
{
	if (ssa::bindingsVersion() != s_tableVersion)
	{
		s_tableVersion = ssa::bindingsVersion();
		rebuild();
	}
	const Table& t = s_table;
	const double dt = s_last > 0 ? std::clamp(pad.seconds - s_last, 0.0, 0.1) : 0.0;
	s_last = pad.seconds;
	WiiState w;
	if (!pad.connected)
	{
		out = w;
		return;
	}
	// buttons (bt.cpp's layout: the report's first byte in the high half)
	uint16_t b = 0;
	if (on(pad, t.left)) b |= 0x0100;
	if (on(pad, t.right)) b |= 0x0200;
	if (on(pad, t.down)) b |= 0x0400;
	if (on(pad, t.up)) b |= 0x0800;
	if (on(pad, t.plus)) b |= 0x1000;
	if (on(pad, t.two)) b |= 0x0001;
	if (on(pad, t.one)) b |= 0x0002;
	if (on(pad, t.b)) b |= 0x0004;
	if (on(pad, t.a)) b |= 0x0008;
	if (on(pad, t.minus)) b |= 0x0010;
	if (on(pad, t.home)) b |= 0x0080;
	// a PS Move as the Wii Remote: its buttons too (DolphinPS4's: Move A, T B, Cross 1, Circle 2,
	// Select -, Start +; Start + Select recentres its pointer)
	const bool useMove = pad.move && ssa::setting("psmove") == 1;
	if (useMove)
	{
		const uint16_t m = pad.moveButtons;
		if (m & MOVE_MOVE) b |= 0x0008;
		if ((m & MOVE_T) || pad.moveTrigger > 100) b |= 0x0004;
		if (m & MOVE_CROSS) b |= 0x0002;
		if (m & MOVE_CIRCLE) b |= 0x0001;
		if ((m & MOVE_SELECT) && !(m & MOVE_START)) b |= 0x0010;
		if ((m & MOVE_START) && !(m & MOVE_SELECT)) b |= 0x1000;
		if ((m & MOVE_START) && (m & MOVE_SELECT))
			s_recenter = true;
	}
	w.buttons = b;
	w.c = on(pad, t.c);
	w.z = on(pad, t.z);
	// the Nunchuk's stick: the calibration's range (centre 0x80, 0x60 each way), a small dead zone
	{
		float x, y;
		group(pad, t.stick, x, y);
		auto dz = [](float v) { return std::fabs(v) < 0.09f ? 0.0f : std::clamp(v, -1.0f, 1.0f); };
		w.stick[0] = (uint8_t)std::clamp(0x80 + (int)std::lround(dz(x) * 0x60), 0, 255);
		w.stick[1] = (uint8_t)std::clamp(0x80 + (int)std::lround(dz(y) * 0x60), 0, 255);
	}
	// the Wii Remote's motion: from the Move or the DualShock 4 (the pad held like a remote pointing at
	// the TV: x right, y up, z towards the player -> the remote's x, z up, y forward), else at rest;
	// Shake held: a 6 Hz swing of 3 g on top (as Dolphin's Shake)
	float ax = 0, ay = 0, az = 1;
	if (useMove)
	{
		ax = pad.moveAccel[0];
		ay = pad.moveAccel[1];
		az = pad.moveAccel[2];
	}
	else if (ssa::setting("motion") == 1 && pad.motion)
	{
		ax = -pad.accel[0];
		ay = -pad.accel[2];
		az = pad.accel[1];
	}
	const float swing = 3.0f * (float)std::sin(pad.seconds * 6.0 * 6.283185307179586);
	if (on(pad, t.shake))
	{
		ax += swing;
		ay += swing;
		az += swing;
	}
	w.accel[0] = accel8(ax, 0x80, 26);
	w.accel[1] = accel8(ay, 0x80, 26);
	w.accel[2] = accel8(az, 0x80, 26);
	{
		float nx = 0, ny = 0, nz = 1;
		if (on(pad, t.nshake))
		{
			nx += swing;
			ny += swing;
			nz += swing;
		}
		w.nunchukAccel[0] = accel8(nx, 0x80, 51);
		w.nunchukAccel[1] = accel8(ny, 0x80, 51);
		w.nunchukAccel[2] = accel8(nz, 0x80, 51);
	}
	// the pointer
	if (on(pad, t.recenter))
		s_recenter = true;
	if (s_recenter.exchange(false))
		s_px = s_py = 0;
	const float speed = (float)(ssa::setting("pointer_speed") + 1) / 5.0f;
	const int source = ssa::setting("pointer");
	bool shown = true;
	if (useMove)
	{
		// the Move's gyroscope (yaw about its z, pitch about its x): ~40 degrees across the screen
		s_px -= pad.moveGyro[2] * 2.8f * speed * (float)dt;
		s_py += pad.moveGyro[0] * 2.8f * speed * (float)dt;
	}
	else if (source == 0)
	{
		// the Pointer binding's stick, moving the pointer (it stays where it was left)
		float x, y;
		group(pad, t.pointer, x, y);
		if (std::fabs(x) < 0.15f) x = 0;
		if (std::fabs(y) < 0.15f) y = 0;
		s_px += x * 1.6f * speed * (float)dt;
		s_py += y * 1.6f * speed * (float)dt;
	}
	else if (source == 1 && pad.motion)
	{
		// the DualShock 4's gyroscope: yaw about y, pitch about x
		s_px -= pad.gyro[1] * 2.8f * speed * (float)dt;
		s_py += pad.gyro[0] * 2.8f * speed * (float)dt;
	}
	else if (source == 2)
	{
		// the touchpad: where the finger is (the pointer stays when it lifts)
		if (pad.touch)
		{
			s_px = pad.touchX * 2.0f - 1.0f;
			s_py = 1.0f - pad.touchY * 2.0f;
		}
	}
	else
		shown = false;
	s_px = std::clamp(s_px, -1.0f, 1.0f);
	s_py = std::clamp(s_py, -1.0f, 1.0f);
	w.pointer = shown;
	w.px = s_px;
	w.py = s_py;
	out = w;
}

const char* wiimapCapture(const PadState& pad)
{
	for (const InName& n : kNames)
		if (n.bit && (pad.buttons & n.bit))
			return n.name;
	if (pad.l2 > 160)
		return "L2";
	if (pad.r2 > 160)
		return "R2";
	const struct { uint8_t v; const char* neg; const char* pos; } axes[] = {
		{pad.lx, "Left X-", "Left X+"}, {pad.ly, "Left Y-", "Left Y+"}, {pad.rx, "Right X-", "Right X+"}, {pad.ry, "Right Y-", "Right Y+"}};
	for (const auto& a : axes)
	{
		if (a.v < 40)
			return a.neg;
		if (a.v > 215)
			return a.pos;
	}
	return "";
}

bool wiimapAtRest(const PadState& pad)
{
	auto centred = [](uint8_t v) { return v > 80 && v < 176; };
	return !pad.buttons && pad.l2 < 40 && pad.r2 < 40 && centred(pad.lx) && centred(pad.ly) && centred(pad.rx) && centred(pad.ry);
}

const char* wiimapDescribe(const char* input)
{
	if (!input || !*input)
		return "None";
	for (const InName& n : kNames)
		if (!strcmp(n.name, input))
			return n.desc;
	return input;
}
