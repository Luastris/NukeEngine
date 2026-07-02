#pragma once
#ifndef NUKEE_ISCRIPT_H
#define NUKEE_ISCRIPT_H

namespace nuke {

// The SCRIPTING service contract (unified plugin model). The active scripting backend
// (NukeScript/Lua today, C#/Mono later — roadmap 1.2) implements this and hands it to
// the loader via NUKEModule::queryService(); one active provider at a time (loader-
// enforced). Consumers reach it through GetService<iScript>() or the null-safe
// Script facade (API/Model/Script.h).
//
// Threading: Run executes in the game VM — call it from the update/render thread the
// scripts run on (both hosts drive Update + scripts on one thread), never from workers.
class iScript
{
public:
	static constexpr const char* kServiceName = "scripting";

	virtual ~iScript() {}

	// Backend language id, e.g. "lua".
	virtual const char* Language() = 0;

	// Execute a source snippet in the shared game VM. `chunkName` labels errors/logs.
	// False on load or runtime error (the backend logs the details).
	virtual bool Run(const char* code, const char* chunkName) = 0;
};

}  // namespace nuke

#endif // !NUKEE_ISCRIPT_H
