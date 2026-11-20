// The app's settings: what the quick menu changes (graphics, performance, the Wii Remote, the control
// bindings), kept in one file (/data/SSA/settings.ini on the PS4; SSA_SETTINGS elsewhere) and read by
// the renderer, the input thread and the game thread. A setting is one of a fixed list of values (an
// index, read lock-free); a binding is the DualShock 4 input a Wii Remote / Nunchuk control comes from
// (DolphinPS4's names: "Cross", "R2", "Pad N", "Left Stick"...).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace ssa
{
struct SettingDef
{
	const char* key;   // in the file: key=value
	const char* label; // in the menu
	const char* page;  // "Graphics", "Performance", "Wii Remote", "Main"
	std::vector<const char*> values; // what the file holds
	std::vector<const char*> names;  // what the menu shows
	int def;           // the default's index
	bool live;         // takes effect at once (else at the next start: the menu says so)
	const char* help;  // one line under the list
};

const std::vector<SettingDef>& settingDefs();
const SettingDef* settingDef(const char* key);
int setting(const char* key);            // the value's index (the default for an unknown key)
const char* settingValue(const char* key);
void setSetting(const char* key, int index);
// which value the setting started the app with (next-start settings: what is in effect)
int settingAtStart(const char* key);

// the Wii Remote / Nunchuk controls the bindings page lists, in order
struct BindingDef
{
	const char* key;     // "A", "Pointer", "Stick"...
	const char* label;   // "A", "Pointer", "Nunchuk Stick"...
	bool stickGroup;     // a whole stick (Left Stick / Right Stick / D-Pad), chosen with left/right
	const char* def;     // DolphinPS4's default
};
const std::vector<BindingDef>& bindingDefs();
std::string binding(const char* key);
void setBinding(const char* key, const std::string& input);
void resetBindings();
uint32_t bindingsVersion(); // changes on every binding change (readers rebuild their tables)

// the file: read once at start (before anything reads a setting), written after each change
void settingsLoad(const char* path);
void settingsSave();
}
