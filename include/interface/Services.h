#pragma once
#ifndef NUKE_SERVICES_H
#define NUKE_SERVICES_H
#include "NukeAPI.h"

namespace nuke {

// Engine service registry (unified plugin model). A "service" is a named engine-wide
// contract ("render", "physics", "audio", "scripting", ...) with exactly ONE active
// provider at a time — the loader (Modular.cpp) enforces mutual exclusion, so lookups
// are unambiguous. Providers are plugin-supplied interface instances: the loader
// registers NUKEModule::queryService() under NUKEModule::provides() after OnLoad() and
// revokes it before Shutdown(), so a pointer is only ever visible while its plugin is
// live. Consumers must therefore not cache the pointer across plugin toggles — re-query.
NUKEENGINE_API void  Services_Provide(const char* service, void* iface);
NUKEENGINE_API void  Services_Revoke(const char* service);
NUKEENGINE_API void* Services_GetRaw(const char* service);

// Typed lookup: the interface declares its own service name (e.g. iRender::kServiceName).
// Returns null if no provider is currently active — callers handle "service off".
template <class T>
inline T* GetService() { return static_cast<T*>(Services_GetRaw(T::kServiceName)); }

}  // namespace nuke

#endif // !NUKE_SERVICES_H
