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

// The loaded plugins live as a SINGLE instance inside the engine DLL. Access them via
// GetModules() — a header-level `static` would give every TU (e.g. the editor) its own
// empty copy, so the plugin manager would never see what InitModules loaded.
NUKEENGINE_API bc::vector<std::shared_ptr<NUKEModule>>& GetModules();

// The ABI level a discovered module's DLL was built against (its exported nuke_module_abi;
// 1 for DLLs that predate the stamp). GUARD every call to a vtable-appended NUKEModule
// virtual with this — e.g. `ModuleAbi(m) >= 2 && m->editorTool()` — so a stale project
// module degrades to the virtual's default instead of crashing the host (its shorter
// vtable has no such slot). Levels are documented beside NUKE_MODULE_ABI.
NUKEENGINE_API int ModuleAbi(const NUKEModule* m);

// InitModules now only DISCOVERS plugins (imports them for metadata) into the shared pool —
// it does NOT activate them. Activate the project's chosen plugins with EnablePlugin().
NUKEENGINE_API void InitModules(AppInstance* instance);
NUKEENGINE_API void UnloadModules();

// Discover plugins from ONE extra directory into the shared pool (same import as
// InitModules; a DLL whose file name is already in the pool is skipped — the host's own
// modules/ wins). This is how PROJECT-LOCAL game modules (<project>/modules, Phase 6.0)
// join the pool: the editor calls it at project open and after a game-module build.
NUKEENGINE_API void DiscoverModulesIn(const std::string& dir);

// C++ game-module rebuild cycle (Phase 6.0). A discovered DLL is file-LOCKED for the whole
// session (the pool holds its boost::dll library), so a rebuild must first UNLOAD it:
//   UnloadModuleDll(file)  — disable if loaded (components -> UnknownComponent placeholders)
//                            and DROP it from the pool; the last reference frees the DLL and
//                            the build can overwrite the file. Refuses PHASE_BOOT providers.
//                            NOTE: the caller must not hold shared_ptr copies (plugin window
//                            selection etc.) or the file stays locked.
//   DiscoverModuleFile(p)  — import ONE dll path back into the pool (returns null on failure;
//                            replaces nothing — call UnloadModuleDll first). Enable the
//                            result with EnablePlugin: live UnknownComponent placeholders of
//                            its types restore automatically.
NUKEENGINE_API bool        UnloadModuleDll(const std::string& moduleFile);
NUKEENGINE_API NUKEModule* DiscoverModuleFile(const std::string& absPath);

// Activate / deactivate a discovered plugin at any time (the manager's checkboxes + the
// per-project load list drive these). Enable = OnLoad() (sync, registers types) +
// service registration (queryService -> Services_Provide) + Run() (background).
// Disable = service revoke + Shutdown(). Idempotent.
// Mutual exclusion: enabling a service provider first disables the current provider of
// that service — EXCEPT when either side is PHASE_BOOT (the renderer): boot providers
// can't be torn down mid-run, so EnablePlugin refuses and the UI persists the choice as
// "applies after restart".
NUKEENGINE_API void EnablePlugin(NUKEModule* m);
NUKEENGINE_API void DisablePlugin(NUKEModule* m);

// The currently LOADED provider of a service, or null if the service is off.
NUKEENGINE_API NUKEModule* ActiveServiceProvider(const char* service);

// Pick a provider of `service` from the discovered pool: the one whose moduleFile equals
// preferredFile if present, else the first provider found (fallback, logged). Null if the
// pool has no provider of that service. Does NOT enable it.
NUKEENGINE_API NUKEModule* FindServiceProvider(const char* service,
                                               const std::string& preferredFile = "");

// Load the engine's built-in shader files from `dir` (e.g. "shaders") and push their source
// into the renderer (render->setShaderSource) BEFORE render->init(). Each "<name>.hlsl" is
// registered under "<name>" (so "world.vs.hlsl" -> "world.vs"). The ENGINE does the file IO;
// the render module stays free of file/boost dependencies.
class iRender;
NUKEENGINE_API void LoadBuiltinShaders(iRender* render, const std::string& dir);
// Packed variant (3.2): built-ins from the mounted game.nupak ("shaders/" entries) — the
// dist ships no loose shaders/ dir; mods override entries through the Package layers.
NUKEENGINE_API void LoadBuiltinShadersPackaged(iRender* render);

// Which plugin (dll name) provides a component type, "" for engine built-ins. Learned by
// diffing the reflection registry around each plugin's OnLoad().
NUKEENGINE_API const char* PluginForType(const std::string& type);

// True if a type's components should be live (built-in, or its plugin is currently loaded);
// false means load them as inert UnknownComponent placeholders.
NUKEENGINE_API bool IsTypeActive(const std::string& type);

}  // namespace nuke

#endif // !NUKE_MODULAR_H

