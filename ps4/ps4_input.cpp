// The DualShock 4 (and, when it is on in the quick menu, a PS Move) as the emulated Wii Remote +
// Nunchuk, and the quick menu's pad. A thread of its own reads them every 4 ms (scePadReadState is a
// system call, ~60 us: on the game thread it cost ~1.6% of it, v00.14). Each read goes to the menu first
// (overlayInput: while it is open, and until the pad rests after it closes, the game sees a neutral
// pad), then wiimapUpdate makes the Wii Remote's state from it (the bindings, motion, the pointer),
// which the game thread takes (bt.cpp: g_btReadInput). The Wii Remote's rumble (bt.cpp: g_btRumble)
// goes to the DualShock 4 (and the Move) from here.
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <atomic>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "overlay.h"
#include "settings.h"
#include "wiimap.h"

void ssaPinThread(int core, int priority, const char* name); // ps4_threads.cpp
extern bool (*g_btReadInput)(WiiState& out);                 // bt.cpp
extern void (*g_btRumble)(bool on);

// libScePad / libSceMove calls OpenOrbis has only as stubs without parameters (DolphinPS4's
// declarations; the Move's state layout follows shadPS4's)
extern "C" int32_t ps4_pad_set_motion(int32_t handle, bool enable) __asm__("scePadSetMotionSensorState");
extern "C" int32_t ps4_move_init() __asm__("sceMoveInit");
extern "C" int32_t ps4_move_open(int32_t user, int32_t type, int32_t index) __asm__("sceMoveOpen");
extern "C" int32_t ps4_move_read(int32_t handle, void* data) __asm__("sceMoveReadStateLatest");
extern "C" int32_t ps4_move_light(int32_t handle, uint8_t r, uint8_t g, uint8_t b) __asm__("sceMoveSetLightSphere");
extern "C" int32_t ps4_move_vibration(int32_t handle, uint8_t motor) __asm__("sceMoveSetVibration");

namespace
{
struct MoveState
{
	float accelerometer[3];
	float gyro[3];
	uint16_t buttons;
	uint16_t trigger;
	uint16_t extension[7];
	uint8_t custom[5];
	int64_t timestamp;
	int32_t count;
	float temperature;
	uint8_t spare[64]; // (in case the system's structure is larger than shadPS4's)
};

int32_t s_user = -1, s_handle = -1, s_motionResult = -1;
float s_touchW = 1920.0f, s_touchH = 943.0f;
int32_t s_move = -1;     // the Move's handle (opened when the setting turns on)
bool s_moveTried = false;
bool s_moveStarted = false; // its library loaded and started (sceMoveOpen can be tried)
bool s_moveMarked = false;  // the mark file is there (until the first read)
double s_moveRetryAt = 0;   // no handle: sceMoveOpen again from then
int s_moveOpenFails = 0;
constexpr const char* kMoveMark = "/data/SSA/psmove-starting";

// the Wii Remote's state for the game thread: a sequence lock (odd while it is written)
std::atomic<uint32_t> s_seq{0};
WiiState s_wii;
std::atomic<bool> s_rumbleWanted{false};
bool s_rumbleOn = false;

double seconds()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void publish(const WiiState& w)
{
	const uint32_t s = s_seq.load(std::memory_order_relaxed);
	s_seq.store(s + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	s_wii = w;
	std::atomic_thread_fence(std::memory_order_release);
	s_seq.store(s + 2, std::memory_order_release);
}

bool readWii(WiiState& out)
{
	for (int tries = 0; tries < 64; tries++)
	{
		const uint32_t s0 = s_seq.load(std::memory_order_acquire);
		if (s0 & 1)
			continue;
		WiiState w = s_wii;
		std::atomic_thread_fence(std::memory_order_acquire);
		if (s_seq.load(std::memory_order_relaxed) == s0)
		{
			out = w;
			return true;
		}
	}
	return false;
}

void rumble(bool on)
{
	s_rumbleWanted.store(on, std::memory_order_relaxed);
}

void open()
{
	sceUserServiceInitialize(nullptr);
	scePadInit();
	sceUserServiceGetInitialUser(&s_user);
	s_handle = scePadOpen(s_user, 0, 0, nullptr);
	fprintf(stderr, "input: pad of user %x: handle %d\n", (unsigned)s_user, (int)s_handle);
	if (s_handle < 0)
		return;
	OrbisPadInformation info{};
	if (scePadGetControllerInformation(s_handle, &info) == 0 && info.touchResolutionX && info.touchResolutionY)
	{
		s_touchW = info.touchResolutionX;
		s_touchH = info.touchResolutionY;
	}
	s_motionResult = ps4_pad_set_motion(s_handle, true);
	fprintf(stderr, "input: touchpad %.0fx%.0f, motion sensors on: %#x\n", s_touchW, s_touchH, (unsigned)s_motionResult);
}

// the PS Move: the library is touched only once the setting is on; each step logged first. Its system
// module (libSceMove) is not one the app loads at start, and a call into a library whose module isn't
// loaded ends the app without a word: v00.18-v00.20 died in sceMoveInit that way at every start once
// the setting was on (DolphinPS4 v03.20 at boot). So sceSysmoduleLoadModule first. And a mark file
// while it starts, until the first read: still there at the next start, that start ended the app, and
// the setting goes off instead of ending it again.
void tryOpenMove(double now)
{
	s_move = ps4_move_open(s_user, 0, 0);
	if (s_move >= 0)
	{
		fprintf(stderr, "input: PS Move: handle %#x\n", (unsigned)s_move);
		ps4_move_light(s_move, 0, 90, 255); // (player 1's blue)
		if (s_moveOpenFails)
			overlayToast("PS Move connected");
		return;
	}
	if (s_moveOpenFails < 3)
		fprintf(stderr, "input: PS Move: sceMoveOpen %#x (again in 3 s)\n", (unsigned)s_move);
	if (s_moveOpenFails++ == 0)
		overlayToast("No PS Move found (looking for one every 3 s)", 4000);
	s_moveRetryAt = now + 3.0;
}

void openMove(double now)
{
	s_moveTried = true;
	if (access(kMoveMark, F_OK) == 0)
	{
		unlink(kMoveMark);
		fprintf(stderr, "input: PS Move: starting it ended the app the last time: the setting turned off\n");
		ssa::setSetting("psmove", 0);
		ssa::settingsSave();
		overlayToast("PS Move turned off: starting it ended the game last time", 8000);
		s_moveTried = false; // (turned on again from the menu: tried again)
		return;
	}
	if (FILE* f = fopen(kMoveMark, "w"))
	{
		fclose(f);
		s_moveMarked = true;
	}
	fprintf(stderr, "input: PS Move: loading its system module\n");
	const int32_t m = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOVE);
	fprintf(stderr, "input: PS Move: module %#x\n", (unsigned)m);
	if (m != 0)
	{
		overlayToast("No PS Move (its system library did not load)", 4000);
		return;
	}
	const int32_t r = ps4_move_init();
	fprintf(stderr, "input: PS Move: init %#x, open user %x\n", (unsigned)r, (unsigned)s_user);
	if (r != 0)
	{
		overlayToast("No PS Move (its library did not start)", 4000);
		return;
	}
	s_moveStarted = true;
	tryOpenMove(now);
}

void readOnce()
{
	PadState p;
	p.seconds = seconds();
	OrbisPadData d{};
	const int32_t r = s_handle >= 0 ? scePadReadState(s_handle, &d) : -1;
	static int s_errors = 0;
	if ((r != 0 || !d.connected) && s_errors < 5)
	{
		s_errors++;
		fprintf(stderr, "input: scePadReadState %#x, connected %d\n", (unsigned)r, (int)d.connected);
	}
	if (r == 0 && d.connected)
	{
		p.connected = true;
		p.buttons = d.buttons;
		p.lx = d.leftStick.x;
		p.ly = d.leftStick.y;
		p.rx = d.rightStick.x;
		p.ry = d.rightStick.y;
		p.l2 = d.analogButtons.l2;
		p.r2 = d.analogButtons.r2;
		// (OpenOrbis's OrbisPadData has the two vectors swapped: "vel" is the acceleration in g, "acell"
		// the rotation in rad/s - DolphinPS4, checked on an official DualShock 4)
		p.motion = s_motionResult == 0;
		p.accel[0] = d.vel.x;
		p.accel[1] = d.vel.y;
		p.accel[2] = d.vel.z;
		p.gyro[0] = d.acell.x;
		p.gyro[1] = d.acell.y;
		p.gyro[2] = d.acell.z;
		if (d.touch.fingers > 0)
		{
			p.touch = true;
			p.touchX = d.touch.touch[0].x / s_touchW;
			p.touchY = d.touch.touch[0].y / s_touchH;
		}
		// (the first 100 changes of the buttons, for diagnosis)
		static uint32_t s_lastRaw = 0;
		static int s_changes = 0;
		if (d.buttons != s_lastRaw && s_changes < 100)
		{
			s_changes++;
			fprintf(stderr, "input: DS4 buttons %08x\n", (unsigned)d.buttons);
		}
		s_lastRaw = d.buttons;
	}
	if (ssa::setting("psmove") == 1)
	{
		if (!s_moveTried)
			openMove(p.seconds);
		else if (s_moveStarted && s_move < 0 && p.seconds >= s_moveRetryAt)
			tryOpenMove(p.seconds);
		MoveState m{};
		const int32_t mr = s_move >= 0 ? ps4_move_read(s_move, &m) : -1;
		if (s_moveMarked) // (it started and the first read was made: the mark goes)
		{
			s_moveMarked = false;
			unlink(kMoveMark);
			fprintf(stderr, "input: PS Move: started, first read %#x\n", (unsigned)mr);
		}
		if (mr == 0)
		{
			p.move = true;
			p.moveButtons = m.buttons;
			p.moveTrigger = m.trigger;
			memcpy(p.moveAccel, m.accelerometer, sizeof(p.moveAccel));
			memcpy(p.moveGyro, m.gyro, sizeof(p.moveGyro));
			static uint16_t s_lastMove = 0;
			static int s_moveLogs = 0;
			if (m.buttons != s_lastMove && s_moveLogs < 40)
			{
				s_moveLogs++;
				fprintf(stderr, "input: PS Move buttons %#06x trigger %u, accel %.2f %.2f %.2f\n", m.buttons, m.trigger, m.accelerometer[0],
					m.accelerometer[1], m.accelerometer[2]);
			}
			s_lastMove = m.buttons;
		}
	}
	// the quick menu first: while it has the pad the game sees a neutral one
	WiiState w;
	if (overlayInput(p))
	{
		PadState rest;
		rest.connected = p.connected;
		rest.seconds = p.seconds;
		wiimapUpdate(rest, w);
	}
	else
		wiimapUpdate(p, w);
	publish(w);
	// rumble (not while the menu is open, and not with the setting off)
	const bool want = s_rumbleWanted.load(std::memory_order_relaxed) && ssa::setting("rumble") == 0 && !overlayOpen();
	if (want != s_rumbleOn)
	{
		s_rumbleOn = want;
		if (s_handle >= 0)
		{
			OrbisPadVibeParam v{(uint8_t)(want ? 220 : 0), (uint8_t)(want ? 140 : 0)};
			scePadSetVibration(s_handle, &v);
		}
		if (s_move >= 0)
			ps4_move_vibration(s_move, want ? 200 : 0);
	}
}

void* padThread(void*)
{
	ssaPinThread(5, 600, "input");
	open();
	for (;;)
	{
		readOnce();
		usleep(4000);
	}
	return nullptr;
}
} // namespace

// at start (ps4_main.cpp, after the settings are read): the reader thread, and bt.cpp's hooks
void ps4InputStart()
{
	g_btReadInput = readWii;
	g_btRumble = rumble;
	pthread_t t;
	if (pthread_create(&t, nullptr, padThread, nullptr) != 0)
		fprintf(stderr, "input: no reader thread\n");
}
