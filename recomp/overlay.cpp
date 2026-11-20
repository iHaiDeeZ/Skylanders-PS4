// The quick menu and the frame counter (overlay.h). The look is DolphinPS4's in-game menu
// (DolphinNoGUI/PS4IngameMenu.cpp) and its Controls panel (PS4XMB.cpp), and the DualShock 4 icons are
// its PS4Glyphs.cpp (Copyright 2026 Dolphin Emulator Project, GPL-2.0-or-later, as this file).
#include "overlay.h"

#include "settings.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <float.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

std::atomic<int> g_ssaPaused{0}; // the menu is open: the game thread waits (ssa_run.cpp pauseGame), the GX thread presents it

namespace
{
using Clock = std::chrono::steady_clock;
// SSA_MENU_SCRIPT (the PC's menu pictures, overlayScriptTick): time runs at 30 presents a second
bool s_scripted = false;
int s_scriptFrame = 0;
Clock::time_point s_scriptStart;
Clock::time_point nowTime()
{
	if (!s_scripted)
		return Clock::now();
	return s_scriptStart + std::chrono::milliseconds((int64_t)s_scriptFrame * 1000 / 30);
}

// ---- the DualShock 4's icons (DolphinPS4's PS4Glyphs) ----
namespace glyphs
{
enum class Kind { None, Cross, Circle, Square, Triangle, Shoulder, Trigger, StickPress, StickDir, Stick, DPadDir, DPad, Options, Touchpad };
struct Info
{
	Kind kind = Kind::None;
	const char* label = "";
	int dir = -1;
	bool analog = false;
};

Info lookup(const std::string& name)
{
	struct Entry { const char* name; Info info; };
	static const Entry kEntries[] = {
		{"Cross", {Kind::Cross}}, {"Circle", {Kind::Circle}}, {"Square", {Kind::Square}}, {"Triangle", {Kind::Triangle}},
		{"L1", {Kind::Shoulder, "L1"}}, {"R1", {Kind::Shoulder, "R1"}},
		{"L2", {Kind::Trigger, "L2", -1, true}}, {"R2", {Kind::Trigger, "R2", -1, true}},
		{"L3", {Kind::StickPress, "L"}}, {"R3", {Kind::StickPress, "R"}},
		{"Options", {Kind::Options}}, {"Touchpad", {Kind::Touchpad}},
		{"Pad N", {Kind::DPadDir, "", 0}}, {"Pad S", {Kind::DPadDir, "", 1}}, {"Pad W", {Kind::DPadDir, "", 2}}, {"Pad E", {Kind::DPadDir, "", 3}},
		{"Left Y-", {Kind::StickDir, "L", 0}}, {"Left Y+", {Kind::StickDir, "L", 1}}, {"Left X-", {Kind::StickDir, "L", 2}}, {"Left X+", {Kind::StickDir, "L", 3}},
		{"Right Y-", {Kind::StickDir, "R", 0}}, {"Right Y+", {Kind::StickDir, "R", 1}}, {"Right X-", {Kind::StickDir, "R", 2}}, {"Right X+", {Kind::StickDir, "R", 3}},
		{"Left Stick", {Kind::Stick, "L"}}, {"Right Stick", {Kind::Stick, "R"}}, {"D-Pad", {Kind::DPad}},
	};
	for (const Entry& e : kEntries)
		if (name == e.name)
			return e.info;
	return {};
}

ImU32 colour(int r, int g, int b, float a)
{
	return IM_COL32(r, g, b, (int)(255 * std::clamp(a, 0.0f, 1.0f)));
}

void centredText(ImDrawList* dl, ImFont* font, float size, ImVec2 c, ImU32 col, const char* text)
{
	if (!font || !*text)
		return;
	const ImVec2 e = font->CalcTextSizeA(size, 10000.0f, 0.0f, text);
	dl->AddText(font, size, ImVec2(c.x - e.x * 0.5f, c.y - e.y * 0.5f), col, text);
}

void arrow(ImDrawList* dl, ImVec2 c, float s, int dir, ImU32 col)
{
	const float dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
	const ImVec2 tip(c.x + dx[dir] * s, c.y + dy[dir] * s);
	const ImVec2 base(c.x - dx[dir] * s * 0.4f, c.y - dy[dir] * s * 0.4f);
	const ImVec2 side(dy[dir] * s * 0.8f, dx[dir] * s * 0.8f);
	dl->AddTriangleFilled(tip, ImVec2(base.x + side.x, base.y + side.y), ImVec2(base.x - side.x, base.y - side.y), col);
}

// a D-pad: dirs lit (bits: 1 up, 2 down, 4 left, 8 right)
void dpad(ImDrawList* dl, ImVec2 c, float r, int lit, float a)
{
	const float arm = r * 0.42f;
	const ImU32 dim = colour(255, 255, 255, 0.28f * a), on = colour(255, 255, 255, a);
	const ImVec2 mins[] = {{c.x - arm * 0.5f, c.y - r}, {c.x - arm * 0.5f, c.y + arm * 0.5f}, {c.x - r, c.y - arm * 0.5f}, {c.x + arm * 0.5f, c.y - arm * 0.5f}};
	const ImVec2 maxs[] = {{c.x + arm * 0.5f, c.y - arm * 0.5f}, {c.x + arm * 0.5f, c.y + r}, {c.x - arm * 0.5f, c.y + arm * 0.5f}, {c.x + r, c.y + arm * 0.5f}};
	for (int i = 0; i < 4; i++)
		dl->AddRectFilled(mins[i], maxs[i], (lit >> i) & 1 ? on : dim, r * 0.12f);
	dl->AddRectFilled(ImVec2(c.x - arm * 0.5f, c.y - arm * 0.5f), ImVec2(c.x + arm * 0.5f, c.y + arm * 0.5f), lit == 15 ? on : dim);
}

float width(const std::string& name, float h)
{
	switch (lookup(name).kind)
	{
	case Kind::None: return 0.0f;
	case Kind::Shoulder:
	case Kind::Trigger: return h * 1.35f;
	case Kind::Options: return h * 1.15f;
	case Kind::Touchpad: return h * 1.7f;
	default: return h;
	}
}

float draw(ImDrawList* dl, ImFont* font, const std::string& name, float x, float cy, float h, float a)
{
	const Info info = lookup(name);
	const float w = width(name, h);
	if (info.kind == Kind::None)
		return 0.0f;
	const float r = h * 0.5f;
	const ImVec2 c(x + w * 0.5f, cy);
	const ImU32 white = colour(255, 255, 255, a), disc = colour(20, 24, 34, 0.85f * a), rim = colour(255, 255, 255, 0.35f * a);
	const float line = std::max(1.5f, h * 0.085f);
	switch (info.kind)
	{
	case Kind::Cross:
	case Kind::Circle:
	case Kind::Square:
	case Kind::Triangle:
	{
		dl->AddCircleFilled(c, r, disc, 32);
		dl->AddCircle(c, r, rim, 32, 1.5f);
		const float s = r * 0.48f;
		if (info.kind == Kind::Cross)
		{
			const ImU32 blue = colour(124, 178, 232, a);
			dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), blue, line);
			dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), blue, line);
		}
		else if (info.kind == Kind::Circle)
			dl->AddCircle(c, s, colour(255, 102, 102, a), 28, line);
		else if (info.kind == Kind::Square)
			dl->AddRect(ImVec2(c.x - s * 0.9f, c.y - s * 0.9f), ImVec2(c.x + s * 0.9f, c.y + s * 0.9f), colour(216, 143, 201, a), 0.0f, 0, line);
		else
			dl->AddTriangle(ImVec2(c.x, c.y - s * 1.05f), ImVec2(c.x - s * 1.05f, c.y + s * 0.75f), ImVec2(c.x + s * 1.05f, c.y + s * 0.75f),
				colour(64, 224, 176, a), line);
		break;
	}
	case Kind::Shoulder:
	{
		const ImVec2 p0(x, cy - r * 0.62f), p1(x + w, cy + r * 0.62f);
		dl->AddRectFilled(p0, p1, disc, r * 0.5f);
		dl->AddRect(p0, p1, rim, r * 0.5f, 0, 1.5f);
		centredText(dl, font, h * 0.55f, c, white, info.label);
		break;
	}
	case Kind::Trigger:
	{
		const ImVec2 p0(x, cy - r), p1(x + w, cy + r);
		dl->AddRectFilled(p0, p1, disc, r * 0.7f, ImDrawFlags_RoundCornersTop);
		dl->AddRect(p0, p1, rim, r * 0.7f, ImDrawFlags_RoundCornersTop, 1.5f);
		centredText(dl, font, h * 0.55f, ImVec2(c.x, c.y - (info.analog ? r * 0.12f : 0.0f)), white, info.label);
		if (info.analog)
		{
			const float by = cy + r * 0.62f;
			dl->AddRectFilledMultiColor(ImVec2(x + w * 0.2f, by - line * 0.5f), ImVec2(x + w * 0.8f, by + line * 0.5f), colour(255, 255, 255, 0.15f * a),
				white, white, colour(255, 255, 255, 0.15f * a));
		}
		break;
	}
	case Kind::StickPress:
	case Kind::StickDir:
	case Kind::Stick:
	{
		dl->AddCircleFilled(c, r, disc, 32);
		dl->AddCircle(c, r, rim, 32, 1.5f);
		dl->AddCircle(c, r * 0.68f, colour(255, 255, 255, 0.5f * a), 28, 1.5f);
		if (info.kind == Kind::StickPress)
			centredText(dl, font, h * 0.42f, ImVec2(c.x, c.y - r * 0.05f), white, info.label[0] == 'L' ? "L3" : "R3");
		else
		{
			centredText(dl, font, h * 0.42f, c, white, info.label);
			const float dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
			if (info.kind == Kind::StickDir)
				arrow(dl, ImVec2(c.x + dx[info.dir] * r * 0.84f, c.y + dy[info.dir] * r * 0.84f), r * 0.26f, info.dir, colour(255, 214, 102, a));
			else
				for (int d = 0; d < 4; d++)
					arrow(dl, ImVec2(c.x + dx[d] * r * 0.84f, c.y + dy[d] * r * 0.84f), r * 0.18f, d, colour(255, 255, 255, 0.7f * a));
		}
		break;
	}
	case Kind::DPadDir:
	case Kind::DPad:
		dpad(dl, c, r * 0.95f, info.kind == Kind::DPad ? 15 : 1 << info.dir, a);
		break;
	case Kind::Options:
	{
		const ImVec2 p0(x + w * 0.18f, cy - r * 0.8f), p1(x + w * 0.82f, cy + r * 0.8f);
		dl->AddRectFilled(p0, p1, disc, r * 0.3f);
		dl->AddRect(p0, p1, rim, r * 0.3f, 0, 1.5f);
		for (int i = -1; i <= 1; i++)
			dl->AddLine(ImVec2(c.x - r * 0.3f, cy + i * r * 0.32f), ImVec2(c.x + r * 0.3f, cy + i * r * 0.32f), white, line * 0.8f);
		break;
	}
	case Kind::Touchpad:
	{
		const ImVec2 p0(x, cy - r * 0.7f), p1(x + w, cy + r * 0.7f);
		dl->AddRectFilled(p0, p1, disc, r * 0.25f);
		dl->AddRect(p0, p1, white, r * 0.25f, 0, line * 0.8f);
		dl->AddCircleFilled(ImVec2(c.x, cy), r * 0.14f, white, 12);
		break;
	}
	case Kind::None:
		break;
	}
	return w;
}
} // namespace glyphs

// ---- the menu's model (shared: the input thread builds it, the renderer draws it) ----
enum class Page { Main, Graphics, Performance, Controls, WiiRemote };

struct Row
{
	std::string label, value, key; // key: "set:<setting>", "bind:<binding>", "page:<n>", "act:<action>"; "" = a header
	bool header = false;
	bool note = false;    // a value that takes effect at the next start (amber)
	std::string glyph;    // a binding: the DualShock 4 input, drawn as its icon
};

enum class Hint { Cross, Circle, Square, LeftRight, UpDown };

std::mutex s_mutex;
bool s_ready = false;
bool s_open = false;
Page s_page = Page::Main;
std::vector<Row> s_rows;
int s_sel = 0;
std::string s_subtitle, s_help;
std::vector<std::pair<Hint, std::string>> s_hints;
std::string s_toast;
Clock::time_point s_toastUntil;
bool s_framesSeen = false;      // (the renderer has drawn a frame: toasts before that wait for it)
int s_toastEarlyMs = 0;         // a toast's time from before the first frame (shown from it on)
bool s_capturing = false;      // a binding capture: waiting for the pad to rest, then a press
bool s_captureArmed = false;    // (rested: the next press is taken)
double s_captureStart = 0;
int s_mainSel = 0;              // the main page's selection when coming back from a page

// the renderer's
ImFont* s_bold = nullptr;
ImFont* s_regular = nullptr;
float s_appear = 0, s_scroll = 0;
Clock::time_point s_lastDraw, s_firstDraw;
bool s_drawn = false;

// the input thread's
uint32_t s_prev = 0;
bool s_comboHeld = false;
bool s_block = false;           // after closing: the game sees nothing until the pad rests
int s_repeatDir = 0;            // 1 up, 2 down, 3 left, 4 right
double s_repeatAt = 0;

// the frame counter
std::atomic<uint32_t> s_presents{0};
std::atomic<uint32_t> s_loadGame{0}, s_loadGx{0}, s_loadWait{0}; // (percent x 10)
std::atomic<uint32_t> s_cpuMhz{0};
std::atomic<bool> s_cpuBoost{false};
uint32_t s_fpsPresents = 0;
Clock::time_point s_fpsAt;
float s_fps = 0;

constexpr uint32_t kL1 = 0x400, kL2 = 0x100, kL3 = 0x2, kR1 = 0x800, kR2 = 0x200, kR3 = 0x4, kCombo = kL1 | kL2 | kL3 | kR1 | kR2 | kR3;
constexpr uint32_t kUp = 0x10, kDown = 0x40, kLeft = 0x80, kRight = 0x20, kCross = 0x4000, kCircle = 0x2000, kSquare = 0x8000;

const char* pageTitle(Page p)
{
	switch (p)
	{
	case Page::Main: return "PAUSED";
	case Page::Graphics: return "GRAPHICS";
	case Page::Performance: return "PERFORMANCE";
	case Page::Controls: return "CONTROLS";
	case Page::WiiRemote: return "WII REMOTE";
	}
	return "";
}

const char* pageName(Page p)
{
	switch (p)
	{
	case Page::Graphics: return "Graphics";
	case Page::Performance: return "Performance";
	case Page::WiiRemote: return "Wii Remote";
	default: return "Main";
	}
}

Row settingRow(const ssa::SettingDef& d)
{
	const int v = ssa::setting(d.key);
	Row r{d.label, d.names[v], std::string("set:") + d.key};
	// (a next-start setting changed since this start: amber, with the note)
	if (!d.live && v != ssa::settingAtStart(d.key))
	{
		r.note = true;
		r.value += " (next start)";
	}
	return r;
}

// the rows of the page (s_mutex held)
void buildRows()
{
	std::vector<Row> rows;
	if (s_page == Page::Main)
	{
		rows.push_back({"Resume Game", "", "act:resume"});
		rows.push_back(settingRow(*ssa::settingDef("osd")));
		rows.push_back({"Graphics", "", "page:graphics"});
		rows.push_back({"Performance", "", "page:performance"});
		rows.push_back({"Controls", "", "page:controls"});
		rows.push_back({"Wii Remote", "", "page:wiiremote"});
		rows.push_back({"Quit Game", "", "act:quit"});
		s_hints = {{Hint::Cross, "Select"}, {Hint::LeftRight, "Change"}, {Hint::Circle, "Resume"}};
	}
	else if (s_page == Page::Controls)
	{
		rows.push_back({"Wii Remote", "", "", true});
		bool nunchuk = false;
		for (const ssa::BindingDef& b : ssa::bindingDefs())
		{
			if (!nunchuk && !strcmp(b.key, "C"))
			{
				nunchuk = true;
				rows.push_back({"Nunchuk", "", "", true});
			}
			const std::string input = ssa::binding(b.key);
			Row r{b.label, b.stickGroup ? (input.empty() ? "None" : input) : wiimapDescribe(input.c_str()), std::string("bind:") + b.key};
			r.glyph = input;
			rows.push_back(r);
		}
		rows.push_back({"", "", "", true});
		rows.push_back({"Reset to Default", "", "act:resetbind"});
		s_hints = {{Hint::Cross, "Change"}, {Hint::Square, "Clear"}, {Hint::UpDown, "Move"}, {Hint::Circle, "Back"}};
	}
	else
	{
		const char* page = pageName(s_page);
		for (const ssa::SettingDef& d : ssa::settingDefs())
			if (!strcmp(d.page, page))
				rows.push_back(settingRow(d));
		if (s_page == Page::WiiRemote)
			rows.push_back({"Recenter Pointer", "", "act:recenter"});
		s_hints = {{Hint::LeftRight, "Change"}, {Hint::UpDown, "Move"}, {Hint::Circle, "Back"}};
	}
	s_rows = std::move(rows);
	s_subtitle = pageTitle(s_page);
	if (s_sel >= (int)s_rows.size())
		s_sel = (int)s_rows.size() - 1;
	if (s_sel < 0)
		s_sel = 0;
	while (s_sel < (int)s_rows.size() - 1 && s_rows[s_sel].header)
		s_sel++;
	// the help line: the selected row's
	s_help.clear();
	if (s_capturing && s_sel < (int)s_rows.size())
		s_help = "Press the DualShock 4 button for " + s_rows[s_sel].label + "...  (5 s: cancel)";
	else if (s_sel < (int)s_rows.size())
	{
		const std::string& k = s_rows[s_sel].key;
		if (!k.compare(0, 4, "set:"))
		{
			if (const ssa::SettingDef* d = ssa::settingDef(k.c_str() + 4))
				s_help = d->help;
		}
		else if (!k.compare(0, 5, "bind:"))
		{
			bool group = false;
			for (const ssa::BindingDef& b : ssa::bindingDefs())
				if (k == std::string("bind:") + b.key)
					group = b.stickGroup;
			s_help = group ? "Left/Right: the stick (or the D-pad) for it." : "Cross: press a new button for it. Square: none.";
		}
		else if (k == "act:resetbind")
			s_help = "Back to DolphinPS4's layout.";
		else if (k == "act:recenter")
			s_help = "The pointer back to the middle of the screen.";
		else if (k == "act:quit")
			s_help = "Closes the game.";
		else if (k == "act:resume")
			s_help = "Back to the game (or hold L1 L2 L3 R1 R2 R3 again).";
	}
}

void toastLocked(const std::string& text, int ms = 2200)
{
	s_toast = text;
	s_toastUntil = nowTime() + std::chrono::milliseconds(ms);
	s_toastEarlyMs = s_framesSeen ? 0 : ms;
}

void openMenu()
{
	s_open = true;
	s_page = Page::Main;
	s_sel = 0;
	s_capturing = false;
	buildRows();
	g_ssaPaused = 1;
	fprintf(stderr, "menu: open (the game paused)\n");
}

void closeMenu()
{
	s_open = false;
	s_capturing = false;
	s_block = true;
	g_ssaPaused = 0;
	ssa::settingsSave();
	fprintf(stderr, "menu: closed\n");
}

void move(int dir)
{
	const int n = (int)s_rows.size();
	if (!n)
		return;
	for (int i = 0; i < n; i++)
	{
		s_sel = (s_sel + dir + n) % n;
		if (!s_rows[s_sel].header)
			break;
	}
	buildRows();
}

void change(int dir)
{
	if (s_sel >= (int)s_rows.size())
		return;
	const std::string key = s_rows[s_sel].key;
	if (!key.compare(0, 4, "set:"))
	{
		const ssa::SettingDef* d = ssa::settingDef(key.c_str() + 4);
		if (!d)
			return;
		const int n = (int)d->values.size();
		ssa::setSetting(d->key, (ssa::setting(d->key) + dir + n) % n);
		ssa::settingsSave();
		if (!d->live)
			toastLocked(ssa::setting(d->key) == ssa::settingAtStart(d->key) ? "As it is now" : "From the next start");
	}
	else if (!key.compare(0, 5, "bind:"))
	{
		for (const ssa::BindingDef& b : ssa::bindingDefs())
			if (key == std::string("bind:") + b.key && b.stickGroup)
			{
				static const char* kGroups[] = {"Left Stick", "Right Stick", "D-Pad", ""};
				const std::string now = ssa::binding(b.key);
				int at = 0;
				for (int i = 0; i < 4; i++)
					if (now == kGroups[i])
						at = i;
				ssa::setBinding(b.key, kGroups[(at + dir + 4) % 4]);
				ssa::settingsSave();
			}
	}
	buildRows();
}

void activate()
{
	if (s_sel >= (int)s_rows.size())
		return;
	const std::string key = s_rows[s_sel].key;
	if (key == "act:resume")
		closeMenu();
	else if (key == "act:quit")
	{
		ssa::settingsSave();
		fprintf(stderr, "menu: quit\n");
		exit(0);
	}
	else if (key == "act:recenter")
	{
		wiimapRecenter();
		toastLocked("Pointer recentred");
	}
	else if (key == "act:resetbind")
	{
		ssa::resetBindings();
		ssa::settingsSave();
		toastLocked("Controls back to the default layout");
	}
	else if (!key.compare(0, 5, "page:"))
	{
		s_mainSel = s_sel;
		const std::string p = key.substr(5);
		s_page = p == "graphics" ? Page::Graphics : p == "performance" ? Page::Performance : p == "controls" ? Page::Controls : Page::WiiRemote;
		s_sel = 0;
	}
	else if (!key.compare(0, 5, "bind:"))
	{
		bool group = false;
		for (const ssa::BindingDef& b : ssa::bindingDefs())
			if (key == std::string("bind:") + b.key)
				group = b.stickGroup;
		if (group)
			change(1);
		else
		{
			s_capturing = true;
			s_captureArmed = false;
			s_captureStart = -1;
		}
	}
	else if (!key.compare(0, 4, "set:"))
		change(1);
	buildRows();
}

void back()
{
	if (s_page == Page::Main)
		closeMenu();
	else
	{
		s_page = Page::Main;
		s_sel = s_mainSel;
		buildRows();
	}
}
} // namespace

bool overlayInit(const char* fontDir, unsigned char** pixels, int* w, int* h)
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	const std::string dir = fontDir ? fontDir : ".";
	ImFontConfig cfg;
	cfg.OversampleH = 2;
	cfg.OversampleV = 2;
	// (ImGui asserts on a font file it can't open: looked for first)
	auto exists = [](const std::string& path) {
		FILE* f = fopen(path.c_str(), "rb");
		if (f)
			fclose(f);
		return f != nullptr;
	};
	const std::string boldPath = dir + "/Nunito-ExtraBold.ttf", regularPath = dir + "/Nunito-Regular.ttf";
	if (exists(boldPath) && exists(regularPath))
	{
		s_bold = io.Fonts->AddFontFromFileTTF(boldPath.c_str(), 40.0f, &cfg);
		s_regular = io.Fonts->AddFontFromFileTTF(regularPath.c_str(), 40.0f, &cfg);
	}
	if (!s_bold || !s_regular)
	{
		fprintf(stderr, "menu: no Nunito fonts in %s: ImGui's own\n", dir.c_str());
		io.Fonts->Clear();
		s_bold = s_regular = io.Fonts->AddFontDefault();
	}
	io.Fonts->GetTexDataAsRGBA32(pixels, w, h);
	io.Fonts->SetTexID((ImTextureID)(intptr_t)1);
	std::lock_guard lock(s_mutex);
	s_ready = true;
	fprintf(stderr, "menu: fonts from %s, atlas %dx%d\n", dir.c_str(), *w, *h);
	return true;
}

bool overlayInput(const PadState& pad)
{
	const uint32_t now = pad.connected ? pad.buttons : 0;
	// the sticks as the D-pad, for moving through the menu
	uint32_t dirs = now & (kUp | kDown | kLeft | kRight);
	if (pad.ly < 56) dirs |= kUp;
	if (pad.ly > 200) dirs |= kDown;
	if (pad.lx < 56) dirs |= kLeft;
	if (pad.lx > 200) dirs |= kRight;
	const uint32_t pressed = now & ~s_prev;
	s_prev = now;
	std::lock_guard lock(s_mutex);
	if (!s_ready)
		return false;
	// the combo: all six held (once: let one go before it counts again)
	const bool combo = (now & kCombo) == kCombo;
	if (combo && !s_comboHeld)
	{
		s_comboHeld = true;
		if (s_open)
			closeMenu();
		else
			openMenu();
		s_repeatDir = 0;
		return true;
	}
	if (!combo)
		s_comboHeld = false;
	if (!s_open)
	{
		// after closing: nothing for the game until the pad rests (the Cross that chose Resume)
		if (s_block)
		{
			if (wiimapAtRest(pad))
				s_block = false;
			return true;
		}
		return false;
	}
	if (s_comboHeld)
		return true;
	// a binding capture
	if (s_capturing)
	{
		if (s_captureStart < 0)
			s_captureStart = pad.seconds;
		if (pad.seconds - s_captureStart > 5.0)
		{
			s_capturing = false;
			toastLocked("No button: unchanged");
			buildRows();
			return true;
		}
		if (!s_captureArmed)
		{
			if (wiimapAtRest(pad))
				s_captureArmed = true;
			return true;
		}
		const char* input = wiimapCapture(pad);
		if (*input && s_sel < (int)s_rows.size())
		{
			const std::string key = s_rows[s_sel].key.substr(5);
			ssa::setBinding(key.c_str(), input);
			ssa::settingsSave();
			s_capturing = false;
			s_block = false;
			fprintf(stderr, "menu: %s -> %s\n", key.c_str(), input);
			// (the pressed button doesn't also act in the menu)
			s_prev = now | pad.buttons;
			buildRows();
		}
		return true;
	}
	// directions with repeat (0.4 s, then every 0.08 s)
	int dir = 0;
	if (dirs & kUp) dir = 1;
	else if (dirs & kDown) dir = 2;
	else if (dirs & kLeft) dir = 3;
	else if (dirs & kRight) dir = 4;
	bool step = false;
	if (dir != s_repeatDir)
	{
		s_repeatDir = dir;
		s_repeatAt = pad.seconds + 0.4;
		step = dir != 0;
	}
	else if (dir && pad.seconds >= s_repeatAt)
	{
		s_repeatAt = pad.seconds + 0.08;
		step = true;
	}
	if (step)
	{
		if (dir == 1) move(-1);
		else if (dir == 2) move(1);
		else if (dir == 3) change(-1);
		else if (dir == 4) change(1);
	}
	if (pressed & kCross)
		activate();
	else if (pressed & kCircle)
		back();
	else if ((pressed & kSquare) && s_sel < (int)s_rows.size() && !s_rows[s_sel].key.compare(0, 5, "bind:"))
	{
		ssa::setBinding(s_rows[s_sel].key.c_str() + 5, "");
		ssa::settingsSave();
		buildRows();
	}
	return true;
}

bool overlayOpen()
{
	std::lock_guard lock(s_mutex);
	return s_open;
}

bool overlayWantsFrames()
{
	std::lock_guard lock(s_mutex);
	return s_open || s_appear > 0.01f || nowTime() < s_toastUntil;
}

void overlayToast(const char* text, int ms)
{
	std::lock_guard lock(s_mutex);
	toastLocked(text, ms);
}

void overlayNotePresent()
{
	s_presents++;
}

void overlaySetCpu(unsigned mhz, bool boost)
{
	s_cpuMhz = mhz;
	s_cpuBoost = boost;
}

void overlaySetLoads(float game, float gx, float gpuWait)
{
	s_loadGame = (uint32_t)(game * 10);
	s_loadGx = (uint32_t)(gx * 10);
	s_loadWait = (uint32_t)(gpuWait * 10);
}

ImDrawData* overlayFrame(int W, int H)
{
	if (!s_ready || W <= 0 || H <= 0)
		return nullptr;
	const auto now = nowTime();
	if (!s_drawn)
	{
		s_drawn = true;
		s_firstDraw = s_lastDraw = s_fpsAt = now;
	}
	const float dt = std::clamp(std::chrono::duration<float>(now - s_lastDraw).count(), 0.0f, 0.1f);
	s_lastDraw = now;
	// frames a second, over half-second windows
	{
		const float span = std::chrono::duration<float>(now - s_fpsAt).count();
		if (span >= 0.5f)
		{
			const uint32_t p = s_presents.load();
			if (!g_ssaPaused.load(std::memory_order_relaxed)) // (paused: the last rate stays)
				s_fps = (float)(p - s_fpsPresents) / span;
			s_fpsPresents = p;
			s_fpsAt = now;
		}
	}
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)W, (float)H);
	io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
	ImGui::NewFrame();
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const float s = (float)H / 1080.0f;
	bool any = false;
	{
		std::lock_guard lock(s_mutex);
		s_framesSeen = true;
		if (s_toastEarlyMs > 0) // (set at the start, before anything was on the screen)
		{
			s_toastUntil = now + std::chrono::milliseconds(s_toastEarlyMs);
			s_toastEarlyMs = 0;
		}
		s_appear += ((s_open ? 1.0f : 0.0f) - s_appear) * (1.0f - std::exp(-14.0f * dt));
		const float a = s_appear;
		const bool toast = now < s_toastUntil;
		const float hintTime = std::chrono::duration<float>(now - s_firstDraw).count();
		const float hintA = hintTime < 3.0f || hintTime > 10.0f ? 0.0f : std::min({1.0f, (hintTime - 3.0f) / 0.4f, (10.0f - hintTime) / 0.6f});
		if (a > 0.01f)
		{
			any = true;
			dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H), IM_COL32(0, 0, 0, (int)(150 * a)));
			const float panelW = 860.0f * s;
			const float x0 = -panelW * (1.0f - a);
			dl->AddRectFilledMultiColor(ImVec2(x0, 0), ImVec2(x0 + panelW, (float)H), IM_COL32(8, 20, 46, (int)(240 * a)), IM_COL32(8, 20, 46, (int)(150 * a)),
				IM_COL32(2, 6, 16, (int)(150 * a)), IM_COL32(2, 6, 16, (int)(240 * a)));
			dl->AddLine(ImVec2(x0 + panelW, 0), ImVec2(x0 + panelW, (float)H), IM_COL32(91, 184, 255, (int)(200 * a)), 2.0f * s);
			const ImU32 white = IM_COL32(255, 255, 255, (int)(255 * a)), dim = IM_COL32(255, 255, 255, (int)(165 * a)),
						accent = IM_COL32(120, 196, 255, (int)(255 * a));
			dl->AddText(s_regular, 26 * s, ImVec2(x0 + 70 * s, 90 * s), accent, s_subtitle.c_str());
			dl->AddText(s_bold, 40 * s, ImVec2(x0 + 70 * s, 122 * s), white, "Skylanders: Spyro's Adventure", nullptr, panelW - 120 * s);
			const float rowH = 62.0f * s, top = 220.0f * s, bottom = (float)H - 200.0f * s;
			const float visible = (bottom - top) / rowH;
			const float target = std::clamp((float)s_sel - visible * 0.5f + 1.0f, 0.0f, std::max(0.0f, (float)s_rows.size() - visible));
			s_scroll += (target - s_scroll) * (1.0f - std::exp(-16.0f * dt));
			dl->PushClipRect(ImVec2(x0, top - 10 * s), ImVec2(x0 + panelW, bottom + 10 * s), true);
			for (size_t i = 0; i < s_rows.size(); i++)
			{
				const Row& row = s_rows[i];
				const float y = top + ((float)i - s_scroll) * rowH;
				// (a row half out of the list fades)
				const float edge = std::min({1.0f, (y - top + rowH * 0.4f) / (rowH * 0.4f), (bottom - rowH * 0.4f - y) / (rowH * 0.6f)});
				if (edge <= 0.02f)
					continue;
				const float ra = a * edge;
				const ImU32 white = IM_COL32(255, 255, 255, (int)(255 * ra)), dim = IM_COL32(255, 255, 255, (int)(165 * ra)),
							accent = IM_COL32(120, 196, 255, (int)(255 * ra));
				if (row.header)
				{
					if (!row.label.empty())
					{
						dl->AddText(s_bold, 26 * s, ImVec2(x0 + 70 * s, y + 16 * s), accent, row.label.c_str());
						dl->AddLine(ImVec2(x0 + 70 * s, y + 52 * s), ImVec2(x0 + panelW - 60 * s, y + 52 * s), IM_COL32(120, 196, 255, (int)(70 * ra)), 1.5f * s);
					}
					continue;
				}
				const bool sel = (int)i == s_sel;
				if (sel)
					dl->AddRectFilled(ImVec2(x0 + 40 * s, y - 4 * s), ImVec2(x0 + panelW - 40 * s, y + 54 * s), IM_COL32(91, 184, 255, (int)(70 * ra)), 12 * s);
				dl->AddText(sel ? s_bold : s_regular, (sel ? 36 : 32) * s, ImVec2(x0 + 70 * s, y + 4 * s), sel ? white : dim, row.label.c_str());
				// a binding: its DualShock 4 icon (names without one stay text); a capture: the prompt
				constexpr float kGlyphH = 38.0f;
				const bool capturing = sel && s_capturing;
				const float gw = row.glyph.empty() || capturing ? 0.0f : glyphs::width(row.glyph, kGlyphH * s);
				if (capturing)
				{
					const char* t = "Press a button...";
					const float tw = s_bold->CalcTextSizeA(30 * s, FLT_MAX, 0, t).x;
					dl->AddText(s_bold, 30 * s, ImVec2(x0 + panelW - 80 * s - tw, y + 7 * s), IM_COL32(255, 210, 120, (int)(255 * ra)), t);
				}
				else if (gw > 0.0f)
					glyphs::draw(dl, s_bold, row.glyph, x0 + panelW - 80 * s - gw, y + 25 * s, kGlyphH * s, ra);
				else if (!row.value.empty())
				{
					const float vs = 30 * s;
					const float tw = s_regular->CalcTextSizeA(vs, FLT_MAX, 0, row.value.c_str()).x;
					const ImU32 c = row.note ? IM_COL32(255, 210, 120, (int)((sel ? 255 : 170) * ra)) : sel ? white : dim;
					dl->AddText(s_regular, vs, ImVec2(x0 + panelW - 80 * s - tw, y + 7 * s), c, row.value.c_str());
				}
			}
			dl->PopClipRect();
			// the help line, then the button hints
			if (!s_help.empty())
				dl->AddText(s_regular, 26 * s, ImVec2(x0 + 70 * s, (float)H - 170 * s), dim, s_help.c_str(), nullptr, panelW - 130 * s);
			float hx = x0 + 70 * s;
			const float hy = (float)H - 80 * s;
			for (const auto& [hint, text] : s_hints)
			{
				const float r = 18 * s;
				if (hint == Hint::LeftRight || hint == Hint::UpDown)
				{
					glyphs::dpad(dl, ImVec2(hx + r, hy), r, hint == Hint::LeftRight ? 12 : 3, a);
					hx += r * 2;
				}
				else
					hx += glyphs::draw(dl, s_bold, hint == Hint::Cross ? "Cross" : hint == Hint::Circle ? "Circle" : "Square", hx, hy, r * 2, a);
				hx += 14 * s;
				dl->AddText(s_regular, 28 * s, ImVec2(hx, hy - 17 * s), dim, text.c_str());
				hx += s_regular->CalcTextSizeA(28 * s, FLT_MAX, 0, text.c_str()).x + 40 * s;
			}
		}
		if (toast)
		{
			any = true;
			const float tw = s_bold->CalcTextSizeA(32 * s, FLT_MAX, 0, s_toast.c_str()).x;
			const ImVec2 p((float)W - tw - 120 * s, (float)H - 150 * s);
			dl->AddRectFilled(ImVec2(p.x - 30 * s, p.y - 18 * s), ImVec2(p.x + tw + 30 * s, p.y + 56 * s), IM_COL32(8, 20, 46, 225), 14 * s);
			dl->AddText(s_bold, 32 * s, p, IM_COL32(255, 255, 255, 255), s_toast.c_str());
		}
		if (hintA > 0.0f && s_appear < 0.5f && !toast)
		{
			// "L1 L2 L3 + R1 R2 R3  Menu", bottom right, from 3 s to 10 s after the first frame
			any = true;
			const char* label = "Menu";
			const float ts = 30 * s, gh = 40 * s, gap = 8 * s, pad = 22 * s;
			const char* left[] = {"L1", "L2", "L3"};
			const char* right[] = {"R1", "R2", "R3"};
			float iconsW = 0;
			for (const char* g : left)
				iconsW += glyphs::width(g, gh) + gap;
			for (const char* g : right)
				iconsW += glyphs::width(g, gh) + gap;
			const float plusW = 26 * s;
			const float tw = s_bold->CalcTextSizeA(ts, FLT_MAX, 0, label).x;
			const float rightX = (float)W - 60 * s, bottomY = (float)H - 60 * s, height = 70 * s;
			const float leftX = rightX - pad - tw - 16 * s - iconsW - plusW - pad;
			auto al = [hintA](int v) { return (int)(v * hintA); };
			dl->AddRectFilled(ImVec2(leftX, bottomY - height), ImVec2(rightX, bottomY), IM_COL32(8, 20, 46, al(215)), 16 * s);
			dl->AddRect(ImVec2(leftX, bottomY - height), ImVec2(rightX, bottomY), IM_COL32(120, 196, 255, al(90)), 16 * s, 0, 1.5f * s);
			float x = leftX + pad;
			const float cy = bottomY - height * 0.5f;
			for (const char* g : left)
				x += glyphs::draw(dl, s_bold, g, x, cy, gh, hintA) + gap;
			glyphs::centredText(dl, s_bold, 30 * s, ImVec2(x + plusW * 0.5f - gap * 0.5f, cy), IM_COL32(255, 255, 255, al(255)), "+");
			x += plusW;
			for (const char* g : right)
				x += glyphs::draw(dl, s_bold, g, x, cy, gh, hintA) + gap;
			dl->AddText(s_bold, ts, ImVec2(x + 8 * s, cy - ts * 0.55f), IM_COL32(255, 255, 255, al(255)), label);
		}
	}
	// the frame counter, top left
	const int osd = ssa::setting("osd");
	if (osd > 0)
	{
		float shift; // (right of the menu's panel while it shows)
		{
			std::lock_guard lock(s_mutex);
			shift = 860.0f * s * s_appear;
		}
		any = true;
		char line1[64], line2[128];
		snprintf(line1, sizeof(line1), "%.1f FPS", s_fps);
		const int n2 = snprintf(line2, sizeof(line2), "game %.0f%%  GX %.0f%%  GPU wait %.0f%%  %.1f ms", s_loadGame.load() / 10.0f, s_loadGx.load() / 10.0f,
			s_loadWait.load() / 10.0f, s_fps > 0 ? 1000.0f / s_fps : 0.0f);
		if (s_cpuMhz.load() && n2 > 0 && n2 < (int)sizeof(line2))
			snprintf(line2 + n2, sizeof(line2) - n2, "  CPU %u MHz%s", s_cpuMhz.load(), s_cpuBoost.load() ? " (Boost)" : "");
		const float f1 = 34 * s, f2 = 24 * s;
		float w = s_bold->CalcTextSizeA(f1, FLT_MAX, 0, line1).x;
		float h = f1 + 14 * s;
		if (osd > 1)
		{
			w = std::max(w, s_regular->CalcTextSizeA(f2, FLT_MAX, 0, line2).x);
			h += f2 + 4 * s;
		}
		const ImVec2 p(28 * s + shift, 24 * s);
		dl->AddRectFilled(ImVec2(p.x - 14 * s, p.y - 8 * s), ImVec2(p.x + w + 14 * s, p.y + h), IM_COL32(8, 20, 46, 190), 10 * s);
		const ImU32 fpsColour = s_fps >= 50 ? IM_COL32(120, 230, 140, 255) : s_fps >= 28 ? IM_COL32(255, 220, 120, 255) : IM_COL32(255, 130, 120, 255);
		dl->AddText(s_bold, f1, p, fpsColour, line1);
		if (osd > 1)
			dl->AddText(s_regular, f2, ImVec2(p.x, p.y + f1 + 6 * s), IM_COL32(255, 255, 255, 220), line2);
	}
	ImGui::Render();
	ImDrawData* d = ImGui::GetDrawData();
	return any && d && d->TotalVtxCount > 0 ? d : nullptr;
}

// SSA_MENU_SCRIPT="frame:press,...": the pad for gx_replay's menu pictures (the renderer calls this
// once a present on the PC). A press is buttons joined by + (combo up down left right cross circle
// square triangle l1 l2 l3 r1 r2 r3 options), held for that present only; "shot": a picture of it
// (overlayScriptShot).
namespace
{
struct ScriptStep
{
	int frame;
	uint32_t buttons;
	bool shot;
};
std::vector<ScriptStep> s_script;
}

void overlayScriptTick()
{
	static bool parsed = false;
	if (!parsed)
	{
		parsed = true;
		const char* e = getenv("SSA_MENU_SCRIPT");
		if (!e)
			return;
		s_scripted = true;
		s_scriptStart = Clock::now();
		static const struct { const char* name; uint32_t bits; } kNames[] = {{"combo", kCombo}, {"up", kUp}, {"down", kDown}, {"left", kLeft},
			{"right", kRight}, {"cross", kCross}, {"circle", kCircle}, {"square", kSquare}, {"triangle", 0x1000}, {"l1", kL1}, {"l2", kL2},
			{"l3", kL3}, {"r1", kR1}, {"r2", kR2}, {"r3", kR3}, {"options", 0x8}};
		for (const char* q = e; *q;)
		{
			char* end = nullptr;
			const long frame = strtol(q, &end, 10);
			if (!end || *end != ':')
				break;
			q = end + 1;
			ScriptStep st{(int)frame, 0, false};
			std::string word;
			for (;; q++)
			{
				if (*q == '+' || *q == ',' || !*q)
				{
					if (word == "shot")
						st.shot = true;
					for (const auto& n : kNames)
						if (word == n.name)
							st.buttons |= n.bits;
					word.clear();
					if (*q != '+')
						break;
				}
				else
					word += *q;
			}
			if (*q == ',')
				q++;
			s_script.push_back(st);
		}
		fprintf(stderr, "menu: script of %zu steps\n", s_script.size());
	}
	if (!s_scripted)
		return;
	PadState p;
	p.connected = true;
	p.seconds = s_scriptFrame / 30.0;
	for (const ScriptStep& st : s_script)
		if (st.frame == s_scriptFrame)
			p.buttons |= st.buttons;
	overlayInput(p);
	s_scriptFrame++;
}

bool overlayScriptShot()
{
	for (const ScriptStep& st : s_script)
		if (st.frame == s_scriptFrame - 1 && st.shot)
			return true;
	return false;
}
