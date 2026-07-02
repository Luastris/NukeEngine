#pragma once
#ifndef NUKEE_SCRIPT_H
#define NUKEE_SCRIPT_H
#include "NukeAPI.h"
#include <string>

namespace nuke {

// Game-side facade over the SCRIPTING service — null-safe: everything degrades cleanly
// (false / "") when no scripting provider is enabled, so callers never null-check the
// service themselves. The heavy lifting (ScriptComponent, file loading, the VM) lives in
// the provider plugin (NukeScript); this is for host/game code that needs "run this
// snippet" or "is scripting available" — e.g. a console window or setup hooks.
class NUKEENGINE_API Script
{
public:
	static bool        Available();   // a scripting provider is active
	static std::string Language();    // e.g. "lua"; "" when none

	// Execute a source snippet in the game VM (see iScript::Run for threading rules).
	static bool Run(const std::string& code, const std::string& chunkName = "snippet");
};

}  // namespace nuke

#endif // !NUKEE_SCRIPT_H
