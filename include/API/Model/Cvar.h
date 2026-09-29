#pragma once
#ifndef NUKEE_CVAR_H
#define NUKEE_CVAR_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <functional>
#include <string>
#include <vector>

namespace nuke {

// Console variables: named, typed knobs ("r.vsync", "g.timeScale", "mygame.spawnRate") with a
// default, a description and flags. Two kinds: STORED (Register - the value lives here; Archive
// persists it in config/main.json ["cvars"]) and BOUND (Bind - a view over engine state through a
// getter / setter, so the engine's own knobs read and write live). Any change runs the OnChange
// hooks and emits "cvar.changed" (payload = the name) on the event bus. The dev console reads
// `name`, sets `name value`, lists `cvars [prefix]`; the editor has a Cvars panel.
enum class CvarType { Bool, Int, Float, String };
enum CvarFlags : unsigned
{
	CvarNone     = 0,
	CvarArchive  = 1,   // saved to config/main.json ["cvars"] (a packaged game writes it; the editor exports at packaging)
	CvarCheat    = 2,   // the console may set it only while Console::Enabled() (scripts always can)
	CvarReadOnly = 4,   // no setter (bound without one, or declared)
};

struct CvarInfo
{
	std::string name, description, value, defaultValue;
	CvarType type = CvarType::String;
	unsigned flags = 0;
	bool bound = false;
};

class NUKEENGINE_API Cvars
{
	NUKE_CLASS_NOCREATE(Cvars, Object)
public:
	// ---- script surface (one reflected method per name: the typed native one is RegisterTyped) ----

	// Register a stored cvar; the type comes from the default: true/false -> bool, an integer ->
	// int, a number -> float, else string. Returns false when the name is taken. `archive` = persist.
	[[nuke::func]] static bool        Register(const std::string& name, const std::string& defaultValue, const std::string& description, bool archive);
	[[nuke::func]] static bool        Has(const std::string& name);
	[[nuke::func]] static std::string Get(const std::string& name);         // "" = unknown
	[[nuke::func]] static double      GetNumber(const std::string& name);   // bool -> 0/1, string -> parsed (0 when not a number)
	[[nuke::func]] static bool        GetBool(const std::string& name);
	[[nuke::func]] static bool        Set(const std::string& name, const std::string& value);   // parsed per type; false = unknown / read-only / bad value
	[[nuke::func]] static bool        SetNumber(const std::string& name, double value);
	[[nuke::func]] static bool        Reset(const std::string& name);       // back to the default
	[[nuke::func]] static std::string Default(const std::string& name);
	[[nuke::func]] static std::string Describe(const std::string& name);    // "name = value (default x) [archive] - description"
	[[nuke::func]] static int         Count();
	[[nuke::func]] static std::string NameAt(int i);                        // sorted by name

	// ---- native ----
	static bool RegisterTyped(const std::string& name, CvarType type, const std::string& defaultValue, const std::string& description, unsigned flags);
	// A view over engine state: `get` formats the live value, `set` parses and applies (null = read-only).
	static bool Bind(const std::string& name, CvarType type, const std::string& description, unsigned flags,
	                 std::function<std::string()> get, std::function<bool(const std::string&)> set);
	static bool Unregister(const std::string& name);
	static bool Info(const std::string& name, CvarInfo& out);
	static std::vector<std::string> Names(const std::string& prefix = "");   // sorted; case-insensitive prefix
	// Set with the console's cheat gate and a reason on failure.
	static bool SetFrom(const std::string& name, const std::string& value, bool fromConsole, std::string* error);
	static long long OnChange(const std::string& name, std::function<void(const std::string& name, const std::string& value)> fn);   // "" = every cvar
	static void      RemoveOnChange(long long id);
	// Re-apply config ["cvars"] onto the registered Archive cvars (after a config reload).
	static void ApplyArchive();
	// Parse / format helpers shared with the editor panel.
	static bool ParseValue(CvarType type, const std::string& text, std::string& normalized);
};

}  // namespace nuke

#endif // !NUKEE_CVAR_H
