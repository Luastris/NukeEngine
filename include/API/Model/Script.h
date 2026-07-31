#pragma once
#ifndef NUKEE_SCRIPT_H
#define NUKEE_SCRIPT_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// Game-side facade over the SCRIPTING service — null-safe (false / "" when no provider is
// enabled). Scripting is a SHARED service: several backends (Lua, C#, native) may be live
// at once. For host/game code needing "run this snippet" / "is scripting available".
class NUKEENGINE_API Script
{
public:
	static bool        Available();   // at least one scripting provider is active
	static std::string Language();    // first provider's language ("lua"); "" when none
	static std::vector<std::string> Languages();   // every live backend's language

	// Execute a source snippet. `language` routes to the matching backend ("lua", "cs", ...);
	// "" = the FIRST provider. False when no backend matches or the code fails.
	static bool Run(const std::string& code, const std::string& chunkName = "snippet",
	                const std::string& language = "");
};

}  // namespace nuke

#endif // !NUKEE_SCRIPT_H
