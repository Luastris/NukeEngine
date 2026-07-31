#pragma once
#ifndef NUKEE_ISCRIPT_H
#define NUKEE_ISCRIPT_H

namespace nuke {

// The scripting service contract. SHARED: several backends may be live at once — each
// implements this, hands it to the loader via NUKEModule::queryService() and overrides
// sharedService() = true. Consumers use GetService/GetServices<iScript> or the Script facade.
// Threading: Run executes in the game VM — call it from the script thread, never from workers.
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

	// Enumerate the backend's loadable script classes as newline-joined utf8 into `buf`. Returns
	// the byte count REQUIRED (call with cap 0 to size, then again with a big-enough buffer);
	// 0 = none / not a class-based backend. ABI: appended at the END of the vtable.
	virtual int ListClasses(char* buf, int cap) { (void)buf; (void)cap; return 0; }
};

}  // namespace nuke

#endif // !NUKEE_ISCRIPT_H
