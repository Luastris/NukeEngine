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

	// The COMPONENT type through which this backend's classes are reached on an atom
	// (the backend owns that component, so only it can name it). "" = classes are not
	// hosted by a component. ABI: appended at the END of the vtable.
	virtual const char* HostComponent() { return ""; }

	// The glyph (UTF-8, from interface/IconsFileTypes.h) that stands for this backend's classes
	// in pickers. "" = the consumer picks a generic one. ABI: appended at the END of the vtable.
	virtual const char* Icon() { return ""; }

	// Enumerate the backend's loadable script classes as newline-joined utf8 into `buf`. Returns
	// the byte count REQUIRED (call with cap 0 to size, then again with a big-enough buffer);
	// 0 = none / not a class-based backend. ABI: appended at the END of the vtable.
	virtual int ListClasses(char* buf, int cap) { (void)buf; (void)cap; return 0; }

	// The props of ONE class as newline-joined "name<TAB>kind" utf8 (kind: number|bool|string),
	// same sizing protocol as ListClasses. Answers about a class the backend can see RIGHT NOW
	// (loaded assembly / the script itself); 0 = unknown class or no props. Reflection-driven
	// UI (the animation + sequencer prop pickers) keys animatable script props off this.
	// ABI: appended at the END of the vtable.
	virtual int ListClassProps(const char* cls, char* buf, int cap)
	{ (void)cls; (void)buf; (void)cap; return 0; }

	// The engine plugins a piece of this backend's code needs, newline-joined utf8, same sizing
	// protocol as ListClasses. `path` is a file on disk; the backend decides whether it is one of
	// its own and reads only what it needs — the host must not slurp files to ask a question that
	// concerns code. Only the backend can read its own language, so the host never parses script
	// code: it asks every scripting service and keeps what they answer.
	// 0 = nothing needed, or the file is not this backend's. ABI: appended at the END of the vtable.
	virtual int ModuleDeps(const char* path, char* buf, int cap)
	{ (void)path; (void)buf; (void)cap; return 0; }

	// Is a piece of this backend's code tied to one platform? Same contract as ModuleDeps.
	// 0 = not this backend's file, and the host judges it by its own means; else "any" for
	// portable code, or the platform tag it is bound to. The host has no idea what a managed
	// assembly or a script dialect is — only the backend can tell whether a particular piece of
	// its code stays portable. ABI: appended at the END of the vtable.
	virtual int PlatformOf(const char* path, char* buf, int cap)
	{ (void)path; (void)buf; (void)cap; return 0; }
};

}  // namespace nuke

#endif // !NUKEE_ISCRIPT_H
