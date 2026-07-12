#pragma once
#ifndef NUKEE_ISCRIPT_H
#define NUKEE_ISCRIPT_H

namespace nuke {

// The SCRIPTING service contract (unified plugin model). A SHARED service: several
// backends (NukeScript/Lua, NukeMono/C#, native plugins) may be live at once — each
// implements this, hands it to the loader via NUKEModule::queryService() and overrides
// sharedService() = true. Each backend brings its OWN component types and owns its OWN
// file formats (cookContent) — there is no cross-backend dispatch to do at this seam.
// Consumers reach backends through GetService<iScript>() (first), GetServices<iScript>()
// (all), or the null-safe Script facade (API/Model/Script.h) which routes by Language().
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

	// Enumerate the backend's loadable script CLASSES (C#: the Electron-derived classes of
	// the loaded game assembly) as newline-joined utf8 into `buf`. Returns the byte count
	// REQUIRED (call with cap 0 to size, again with a big-enough buffer); 0 = none / not a
	// class-based backend (Lua). Feeds the editor's class picker — nobody types names.
	// ABI: appended at the END of the vtable (rebuild every script module together).
	virtual int ListClasses(char* buf, int cap) { (void)buf; (void)cap; return 0; }
};

}  // namespace nuke

#endif // !NUKEE_ISCRIPT_H
