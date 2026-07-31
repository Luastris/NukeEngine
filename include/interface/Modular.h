#pragma once
#ifndef NUKE_MODULAR_H
#define NUKE_MODULAR_H
#include "NukeAPI.h"
#include <interface/NUKEEInteface.h>

#define BOOST_FILESYSTEM_VERSION 3
#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>
namespace bfs = boost::filesystem;

#include <boost/container/vector.hpp>
namespace bc = boost::container;


#include <boost/dll.hpp>
#include <memory>                 // boost.dll (1.91+) returns std::shared_ptr from import_symbol
#include <boost/thread.hpp>

#ifdef USE_WINAPI
#include <Windows.h>
#endif // USE_WINAPI

namespace nuke {

// The loaded plugins live as a SINGLE instance inside the engine DLL — a header-level `static`
// would give every TU its own empty copy.
NUKEENGINE_API bc::vector<std::shared_ptr<NUKEModule>>& GetModules();

// The ABI level a discovered module's DLL was built against (1 for DLLs predating the stamp).
// GUARD every call to a vtable-appended NUKEModule virtual with this — e.g.
// `ModuleAbi(m) >= 2 && m->editorTool()` — since an older DLL has no such slot.
NUKEENGINE_API int ModuleAbi(const NUKEModule* m);

// Discover plugins (import for metadata) into the shared pool; does NOT activate them.
NUKEENGINE_API void InitModules(AppInstance* instance);
NUKEENGINE_API void UnloadModules();

// Discover plugins from ONE extra directory into the shared pool. A DLL whose file name is
// already in the pool is skipped, so the host's own modules/ wins.
NUKEENGINE_API void DiscoverModulesIn(const std::string& dir);

// A discovered DLL is file-LOCKED for the whole session, so a rebuild must unload it first.
// Unload refuses PHASE_BOOT providers, and the caller must hold no shared_ptr copies or the file
// stays locked. Discover imports ONE dll path back into the pool (null on failure) and replaces
// nothing, so call UnloadModuleDll first.
NUKEENGINE_API bool        UnloadModuleDll(const std::string& moduleFile);
NUKEENGINE_API NUKEModule* DiscoverModuleFile(const std::string& absPath);

// Activate / deactivate a discovered plugin. Enable = OnLoad() + service registration + Run();
// disable = service revoke + Shutdown(). Idempotent. Enabling a service provider first disables
// the current provider — except when either side is PHASE_BOOT, which EnablePlugin refuses
// (boot providers can't be torn down mid-run; the choice applies after restart).
NUKEENGINE_API void EnablePlugin(NUKEModule* m);
NUKEENGINE_API void DisablePlugin(NUKEModule* m);

// The currently LOADED provider of a service, or null if the service is off.
NUKEENGINE_API NUKEModule* ActiveServiceProvider(const char* service);

// Pick a provider of `service` from the discovered pool: the one whose moduleFile equals
// preferredFile if present, else the first provider found. Null if none. Does NOT enable it.
NUKEENGINE_API NUKEModule* FindServiceProvider(const char* service,
                                               const std::string& preferredFile = "");

// Load the engine's built-in shader files from `dir` and push their source into the renderer
// BEFORE render->init(). Each "<name>.hlsl" registers under "<name>" ("world.vs.hlsl" ->
// "world.vs"). The engine does the file IO so the render module stays free of file dependencies.
class iRender;
NUKEENGINE_API void LoadBuiltinShaders(iRender* render, const std::string& dir);
// Same, from the mounted game.nupak's "shaders/" entries (the dist ships no loose shaders/ dir).
NUKEENGINE_API void LoadBuiltinShadersPackaged(iRender* render);

// Which plugin (dll name) provides a component type, "" for engine built-ins.
NUKEENGINE_API const char* PluginForType(const std::string& type);

// True if a type's components should be live (built-in, or its plugin is currently loaded);
// false means load them as inert UnknownComponent placeholders.
NUKEENGINE_API bool IsTypeActive(const std::string& type);

}  // namespace nuke

#endif // !NUKE_MODULAR_H

