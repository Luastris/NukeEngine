#pragma once
#ifndef NUKEE_DEVCONSOLE_H
#define NUKEE_DEVCONSOLE_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

// The runtime developer console: a drop-down overlay over the game viewport, drawn through
// the neutral GUI() seam (whatever gui backend the project runs renders it; none = hidden).
// Commands are the REFLECTED STATICS — "Game.Quit", "Water.Spill 0 5 0 2" — with a Lua
// fallback for full expressions. Toggle: ` (grave). While open, gameplay ACTIONS are
// suppressed (Input::SetSuppressed). Packaged games default to DISABLED; the game's own
// cheat flow re-enables via Console.SetEnabled(true).
class NUKEENGINE_API Console
{
	NUKE_CLASS_NOCREATE(Console, Object)
public:
	[[nuke::func]] static void SetEnabled(bool on);   // the cheat gate
	[[nuke::func]] static bool Enabled();
	[[nuke::func]] static void Toggle();              // open/close (same as the grave key)
	[[nuke::func]] static bool IsOpen();
	// Run one console line NOW; returns the printable result (also logged, tag "Console").
	[[nuke::func]] static std::string Execute(const std::string& line);

	// Called by the gui backend every runtime GUI frame, inside the game lock (after Ui::Emit).
	static void Emit();
};

}  // namespace nuke
#endif // !NUKEE_DEVCONSOLE_H
