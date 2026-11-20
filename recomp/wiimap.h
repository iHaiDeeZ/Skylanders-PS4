// The DualShock 4 (and a PS Move) as the emulated Wii Remote + Nunchuk: the control bindings
// (settings.h, DolphinPS4's layout), the Wii Remote's motion (from the DualShock 4's or the Move's
// sensors, or the Shake buttons) and its pointer (a stick, the DualShock 4's gyro, the touchpad or the
// Move's gyro). Platform-free: the PS4's input thread fills a PadState, wiimapUpdate makes the state
// bt.cpp's reports carry.
#pragma once
#include <stdint.h>

struct PadState
{
	bool connected = false;
	uint32_t buttons = 0;        // libScePad's bits (ORBIS_PAD_BUTTON_*: L3 0x2, R3 0x4, ...)
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128; // sticks, y down
	uint8_t l2 = 0, r2 = 0;      // the triggers' analog values
	bool motion = false;         // the sensors give readings
	float accel[3] = {0, 1, 0};  // g: x right, y up, z towards the player
	float gyro[3] = {0, 0, 0};   // rad/s about x (pitch), y (yaw), z (roll)
	bool touch = false;          // a finger on the touchpad: where (0..1)
	float touchX = 0.5f, touchY = 0.5f;
	// a PS Move (when on): its buttons (MOVE_*), the T trigger (0..255), accelerometer (g) and gyro
	// (rad/s), with the Move held like a Wii Remote: x right, y towards the ball, z out of the buttons
	bool move = false;
	uint16_t moveButtons = 0, moveTrigger = 0;
	float moveAccel[3] = {0, 0, 1};
	float moveGyro[3] = {0, 0, 0};
	double seconds = 0;          // when it was read
};

// the Move's button bits (the PS3's layout, as DolphinPS4 reads them)
constexpr uint16_t MOVE_SELECT = 1 << 0, MOVE_T = 1 << 1, MOVE_MOVE = 1 << 2, MOVE_START = 1 << 3,
	MOVE_TRIANGLE = 1 << 4, MOVE_CIRCLE = 1 << 5, MOVE_CROSS = 1 << 6, MOVE_SQUARE = 1 << 7;

struct WiiState
{
	uint16_t buttons = 0;            // the Wii Remote's core buttons (bt.cpp's layout: byte0 << 8 | byte1)
	bool c = false, z = false;
	uint8_t stick[2] = {0x80, 0x80};  // the Nunchuk's
	uint8_t accel[3] = {0x80, 0x80, 0x9A}; // the Wii Remote's (8 bits, 0x80 zero, 26 a g)
	uint8_t nunchukAccel[3] = {0x80, 0x80, 0xB3};
	bool pointer = false;            // the pointer is on the screen: where (-1..1, x right, y up)
	float px = 0, py = 0;
};

void wiimapUpdate(const PadState& pad, WiiState& out);
void wiimapRecenter(); // the gyro pointer back to the screen's centre
// DolphinPS4's input names: is this input on in this state (sticks: past half way)?
bool wiimapInputOn(const PadState& pad, const char* input);
// the first input that differs from the resting state, for a binding capture ("" = none yet)
const char* wiimapCapture(const PadState& pad);
bool wiimapAtRest(const PadState& pad);
const char* wiimapDescribe(const char* input); // "Pad N" -> "D-Pad Up", "Left X-" -> "Left Stick Left"...
