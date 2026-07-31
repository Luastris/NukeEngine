#pragma once
#ifndef NUKE_SERVICES_H
#define NUKE_SERVICES_H
#include "NukeAPI.h"
#include <vector>

namespace nuke {

// Engine service registry. A service is a named engine-wide contract ("render", "physics",
// "audio", "scripting", ...). Exclusive services have exactly one active provider; shared ones
// (NUKEModule::sharedService()) may have several. A provider pointer is only visible while its
// plugin is live — consumers must NOT cache pointers across plugin toggles.
NUKEENGINE_API void  Services_Provide(const char* service, void* iface);
NUKEENGINE_API void  Services_Revoke(const char* service);                       // ALL providers
NUKEENGINE_API void  Services_RevokeIface(const char* service, void* iface);     // one provider
NUKEENGINE_API void* Services_GetRaw(const char* service);                       // first provider
NUKEENGINE_API std::vector<void*> Services_GetAllRaw(const char* service);       // every provider

// Typed lookup: the interface declares its own service name (e.g. iRender::kServiceName).
// Returns null if no provider is currently active — callers handle "service off".
template <class T>
inline T* GetService() { return static_cast<T*>(Services_GetRaw(T::kServiceName)); }

// Every live provider of a shared service (registration order). Empty when none.
template <class T>
inline std::vector<T*> GetServices()
{
	std::vector<T*> out;
	for (void* p : Services_GetAllRaw(T::kServiceName)) out.push_back(static_cast<T*>(p));
	return out;
}

}  // namespace nuke

#endif // !NUKE_SERVICES_H
