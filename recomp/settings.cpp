// The app's settings and control bindings (settings.h).
#include "settings.h"

#include <atomic>
#include <map>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace ssa
{
namespace
{
const std::vector<SettingDef> kDefs = {
	{"osd", "Frame Counter", "Main", {"off", "fps", "detail"}, {"Off", "FPS", "Detailed"}, 0, true,
		"The frames shown each second, top left (Detailed: also the game, GX and GPU threads' load)."},
	{"aspect", "Aspect Ratio", "Graphics", {"auto", "4:3", "stretch"}, {"Auto", "4:3", "16:9"}, 0, true,
		"Auto: 16:9 when Game Widescreen is on, else 4:3 with black bars at the sides. 16:9: the whole screen."},
	{"widescreen", "Game Widescreen", "Graphics", {"on", "off"}, {"On", "Off"}, 0, false,
		"On: the game draws for a 16:9 TV itself (the Wii's widescreen setting) and sees more at the sides."},
	{"resolution", "Internal Resolution", "Graphics", {"1", "2", "3"}, {"1x (640x528)", "2x (1280x1056)", "3x (1920x1584)"}, 0, false,
		"How many pixels the game is drawn with. Higher is sharper and asks more of the GPU."},
	{"filter", "Texture Filtering", "Graphics", {"game", "smooth"}, {"Game", "Smooth"}, 0, true,
		"Smooth: every texture filtered bilinearly, also those the game draws pixelated."},
	{"audio", "Sound", "Performance", {"on", "off"}, {"On", "Off"}, 0, true,
		"Off: the game is silent and its voices are not mixed (a little less work for the game's thread)."},
	{"workers", "Vertex Fetch Threads", "Performance", {"0", "1", "2", "3"}, {"0", "1", "2", "3"}, 3, false,
		"Cores that read the game's vertices for the renderer. 3 is fastest."},
	{"gxthread", "Renderer Thread", "Performance", {"on", "off"}, {"On", "Off"}, 0, false,
		"On: the GX renderer runs on its own core. Off is only for testing (much slower)."},
	{"pointer", "Pointer", "Wii Remote", {"stick", "gyro", "touchpad", "off"}, {"Stick (Controls)", "DualShock 4 Gyro", "Touchpad", "Off"}, 0, true,
		"What aims the Wii Remote's pointer. Stick: the Pointer row on the Controls page."},
	{"pointer_speed", "Pointer Speed", "Wii Remote", {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"},
		{"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"}, 4, true, "How fast the stick or the gyro moves the pointer."},
	{"motion", "Motion Controls", "Wii Remote", {"off", "ds4"}, {"Off", "DualShock 4"}, 0, true,
		"DualShock 4: tilting and swinging the controller moves the Wii Remote's (shakes too)."},
	{"psmove", "PS Move", "Wii Remote", {"off", "on"}, {"Off", "On"}, 0, true,
		"On: a PS Move is the Wii Remote too (Move = A, T = B, Cross = 1, Circle = 2, Select = -, Start = +, "
		"its motion and gyro pointer; Start + Select recentres). The DualShock 4 stays the Nunchuk."},
	{"rumble", "Rumble", "Wii Remote", {"on", "off"}, {"On", "Off"}, 0, true, "The Wii Remote's rumble on the DualShock 4 (and the PS Move)."},
};

const std::vector<BindingDef> kBindings = {
	{"A", "A", false, "Cross"},
	{"B", "B", false, "R2"},
	{"1", "1", false, "Square"},
	{"2", "2", false, "Triangle"},
	{"-", "- (Minus)", false, "L1"},
	{"+", "+ (Plus)", false, "Options"},
	{"Home", "Home", false, "Touchpad"},
	{"Shake", "Shake (spin)", false, "R3"},
	{"Pointer", "Pointer", true, "Right Stick"},
	{"Recenter", "Recenter Pointer", false, ""},
	{"Up", "D-Pad Up", false, "Pad N"},
	{"Down", "D-Pad Down", false, "Pad S"},
	{"Left", "D-Pad Left", false, "Pad W"},
	{"Right", "D-Pad Right", false, "Pad E"},
	{"C", "Nunchuk C", false, "L1"},
	{"Z", "Nunchuk Z", false, "L2"},
	{"Stick", "Nunchuk Stick", true, "Left Stick"},
	{"NShake", "Nunchuk Shake", false, "L3"},
};

std::atomic<int> s_values[32];
int s_atStart[32];
std::mutex s_bindMutex;
std::map<std::string, std::string> s_bindings;
std::atomic<uint32_t> s_bindVersion{1};
std::string s_path;
std::mutex s_saveMutex;

int indexOf(const char* key)
{
	for (size_t i = 0; i < kDefs.size(); i++)
		if (!strcmp(kDefs[i].key, key))
			return (int)i;
	return -1;
}

struct Init
{
	Init()
	{
		for (size_t i = 0; i < kDefs.size(); i++)
			s_values[i] = s_atStart[i] = kDefs[i].def;
		for (const BindingDef& b : kBindings)
			s_bindings[b.key] = b.def;
	}
} s_init;
} // namespace

const std::vector<SettingDef>& settingDefs()
{
	return kDefs;
}

const SettingDef* settingDef(const char* key)
{
	const int i = indexOf(key);
	return i < 0 ? nullptr : &kDefs[i];
}

int setting(const char* key)
{
	const int i = indexOf(key);
	return i < 0 ? 0 : s_values[i].load(std::memory_order_relaxed);
}

const char* settingValue(const char* key)
{
	const int i = indexOf(key);
	return i < 0 ? "" : kDefs[i].values[s_values[i].load(std::memory_order_relaxed)];
}

int settingAtStart(const char* key)
{
	const int i = indexOf(key);
	return i < 0 ? 0 : s_atStart[i];
}

void setSetting(const char* key, int index)
{
	const int i = indexOf(key);
	if (i < 0 || index < 0 || index >= (int)kDefs[i].values.size())
		return;
	s_values[i] = index;
}

const std::vector<BindingDef>& bindingDefs()
{
	return kBindings;
}

std::string binding(const char* key)
{
	std::lock_guard lock(s_bindMutex);
	auto it = s_bindings.find(key);
	return it == s_bindings.end() ? std::string() : it->second;
}

void setBinding(const char* key, const std::string& input)
{
	{
		std::lock_guard lock(s_bindMutex);
		s_bindings[key] = input;
	}
	s_bindVersion++;
}

void resetBindings()
{
	{
		std::lock_guard lock(s_bindMutex);
		for (const BindingDef& b : kBindings)
			s_bindings[b.key] = b.def;
	}
	s_bindVersion++;
}

uint32_t bindingsVersion()
{
	return s_bindVersion.load(std::memory_order_acquire);
}

void settingsLoad(const char* path)
{
	s_path = path ? path : "";
	FILE* f = s_path.empty() ? nullptr : fopen(s_path.c_str(), "r");
	if (!f)
		return;
	char line[512];
	int read = 0;
	while (fgets(line, sizeof(line), f))
	{
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq = 0;
		const char* key = line;
		const char* value = eq + 1;
		if (!strncmp(key, "bind.", 5))
		{
			std::lock_guard lock(s_bindMutex);
			if (s_bindings.count(key + 5))
				s_bindings[key + 5] = value;
			read++;
			continue;
		}
		const int i = indexOf(key);
		if (i < 0)
			continue;
		for (size_t v = 0; v < kDefs[i].values.size(); v++)
			if (!strcmp(kDefs[i].values[v], value))
			{
				s_values[i] = (int)v;
				s_atStart[i] = (int)v;
				read++;
			}
	}
	fclose(f);
	s_bindVersion++;
	fprintf(stderr, "settings: %d values from %s\n", read, s_path.c_str());
}

void settingsSave()
{
	if (s_path.empty())
		return;
	std::lock_guard save(s_saveMutex);
	// (through a new file renamed over the old one: a power cut mid-write never leaves it empty)
	const std::string tmp = s_path + ".new";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return;
	fprintf(f, "# Skylanders: Spyro's Adventure - settings (the quick menu writes this file)\n");
	for (size_t i = 0; i < kDefs.size(); i++)
		fprintf(f, "%s=%s\n", kDefs[i].key, kDefs[i].values[s_values[i].load()]);
	{
		std::lock_guard lock(s_bindMutex);
		for (const BindingDef& b : kBindings)
			fprintf(f, "bind.%s=%s\n", b.key, s_bindings[b.key].c_str());
	}
	fflush(f);
	fclose(f);
	rename(tmp.c_str(), s_path.c_str());
}
}
