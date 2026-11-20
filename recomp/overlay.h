// The quick menu (hold L1 + L2 + L3 + R1 + R2 + R3) and the frame counter, drawn over the game with
// ImGui in DolphinPS4's in-game menu style: a panel on the left, pages for graphics, performance, the
// control bindings (DolphinPS4's list with the DualShock 4's button icons and a press-to-bind capture)
// and the Wii Remote (pointer, motion, PS Move, rumble). The game is paused while it is open. Its
// logic runs on the input thread (overlayInput, every pad read); the renderer draws it (overlayFrame)
// on its own thread; the two share the rows under a lock.
#pragma once
#include "wiimap.h"

struct ImDrawData;

// the fonts (Nunito, from fontDir) and the atlas the renderer uploads (RGBA, w x h)
bool overlayInit(const char* fontDir, unsigned char** pixels, int* w, int* h);
// one pad read (the input thread, ~250 Hz): the menu's combo and its navigation. True while the menu
// has the pad: the game then sees a neutral one.
bool overlayInput(const PadState& pad);
bool overlayOpen();
// something to draw while the game is paused (the menu, its closing, a message): the renderer keeps
// presenting the last frame with it
bool overlayWantsFrames();
// the frame's drawing for a w x h screen (nullptr: nothing); the renderer calls it for every frame
ImDrawData* overlayFrame(int w, int h);
// for the frame counter: a frame went to the screen (the renderer); the threads' loads (once a second)
void overlayNotePresent();
void overlaySetLoads(float game, float gx, float gpuWait);
// the cores' clock for the detailed frame counter (the PS4 app: ~1600 MHz, ~2130 on a PS4 Pro in Boost
// Mode; 0: not shown)
void overlaySetCpu(unsigned mhz, bool boost);
// a message in the corner for a couple of seconds (ms); one from before the first frame is shown from
// the first frame on
void overlayToast(const char* text, int ms = 2200);
// the PC (gx_replay): SSA_MENU_SCRIPT's pad presses, one step a present; a picture wanted of this one
void overlayScriptTick();
bool overlayScriptShot();
