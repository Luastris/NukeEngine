#pragma once
#ifndef NUKEE_SCRIPT_H
#define NUKEE_SCRIPT_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// Game-side facade over the SCRIPTING service — null-safe: everything degrades cleanly
// (false / "") when no scripting provider is enabled, so callers never null-check the
// service themselves. Scripting is a SHARED service: several backends (Lua, C#, native
// plugins) may be live at once — each brings its own component types and file formats;
// this facade is for host/game code that needs "run this snippet" or "is scripting
// available" — e.g. a console window or setup hooks.
class NUKEENGINE_API Script
{
public:
	static bool        Available();   // at least one scripting provider is active
	static std::string Language();    // first provider's language ("lua"); "" when none
	static std::vector<std::string> Languages();   // every live backend's language

	// Execute a source snippet. `language` routes to the matching backend ("lua", "cs",
	// ...); "" = the FIRST provider (legacy single-backend behavior). False when no
	// backend matches or the code fails (the backend logs the details).
	static bool Run(const std::string& code, const std::string& chunkName = "snippet",
	                const std::string& language = "");
};

}  // namespace nuke

#endif // !NUKEE_SCRIPT_H
