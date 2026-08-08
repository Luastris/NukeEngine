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
#include <memory>
#include <string>                 // boost.dll (1.91+) returns std::shared_ptr from import_symbol
#include <boost/thread.hpp>

#ifdef USE_WINAPI
#include <Windows.h>
#endif // USE_WINAPI

namespace nuke {

// The loaded plugins live as a SINGLE instance inside the engine DLL — a header-level `static`
// would give every TU its own empty copy.
NUKEENGINE_API bc::vector<std::shared_ptr<NUKEModule>>& GetModules();

// The dir the run-dir layout (modules/, shaders/, config/) resolves against: the exe's dir,
// except a macOS thin bundle (Foo.app in the run dir, no modules/ of its own) resolves to
// the dir holding the .app. Hosts also chdir here at boot (config is CWD-relative).
NUKEENGINE_API bfs::path RunRoot();

// The ABI level a discovered module's DLL was built against (1 for DLLs predating the stamp).
// GUARD every call to a vtable-appended NUKEModule virtual with this — e.g.
// `ModuleAbi(m) >= 2 && m->editorTool()` — since an older DLL has no such slot.
NUKEENGINE_API int ModuleAbi(const NUKEModule* m);

// SHIELDED queries into a module's vtable. A plugin is foreign code compiled at another time:
// a stale DLL, a shorter vtable or a fault inside the call must degrade to a default, never
// take the editor down. Each checks the ABI level the slot needs and runs behind the same SEH
// guard as OnLoad. Prefer these over calling the virtuals directly from the host.
NUKEENGINE_API bool        ModuleIsEditorTool(NUKEModule* m);   // editorTool(), ABI >= 2
NUKEENGINE_API std::string ModuleCompanionOf(NUKEModule* m);    // companionOf(), ABI >= 3
NUKEENGINE_API bool        ModuleHasSettings(NUKEModule* m);    // HasSettings()
NUKEENGINE_API bool        ModuleDrawSettings(NUKEModule* m);   // Settings(); false = it faulted

// Discover plugins (import for metadata) into the shared pool; does NOT activate them.
NUKEENGINE_API void InitModules(AppInstance* instance);
NUKEENGINE_API void UnloadModules();

// Discover plugins from ONE extra directory into the shared pool. A DLL whose file name is
// already in the pool is skipped, so the host's own modules/ wins.
NUKEENGINE_API void DiscoverModulesIn(const std::string& dir);

// Absolute paths of module DLLs the ABI gate refused during discovery (stale builds). They are
// not in the pool and their types are not registered — rebuild them to get them back.
NUKEENGINE_API std::vector<std::string> RefusedModules();

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

// The RUNNING engine's release name (NukeVersion.h of the build that produced this binary).
// A plugin compares it with its own compiled-against NUKE_ENGINE_VERSION for feature-gating;
// load compatibility stays the ABI stamp's job.
NUKEENGINE_API const char* EngineVersion();

// Same module file across platforms? Compares by lowercase stem ("NukeRenderDiligent.dll" ==
// "NukeRenderDiligent.so") — project files written on one OS keep working on another.
NUKEENGINE_API bool ModuleFileMatches(const std::string& a, const std::string& b);

// Pick a provider of `service` from the discovered pool: the one whose moduleFile equals
// preferredFile if present (cross-platform stem match), else the first provider found.
// Null if none. Does NOT enable it.
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

// Is a plugin INSTALLED? `name` matches the DLL file name (with or without extension) or the
// module title, case-insensitively. Answers from the discovered pool, falling back to the
// modules directory on disk — mods mount before discovery runs. `outLoaded`, when given, also
// reports whether the plugin is currently ACTIVE (installed but disabled is a different case).
NUKEENGINE_API bool ModuleInstalled(const std::string& name, bool* outLoaded = nullptr);

// The engine plugins a native binary links against, read from its import table and filtered to
// plugins this installation has. A mod shipping its own C++ module declares its dependencies
// this way without anyone writing them down. Empty for a non-PE file.
NUKEENGINE_API std::vector<std::string> ModuleImportsOf(const std::string& binaryPath);

// Every reflected type a plugin owns: (type name, plugin file stem). The packager looks for
// these names inside scripts and managed assemblies to learn what a mod's CODE needs.
NUKEENGINE_API std::vector<std::pair<std::string, std::string>> PluginOwnedTypes();

}  // namespace nuke

#endif // !NUKE_MODULAR_H

