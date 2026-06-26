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

// InitModules now only DISCOVERS plugins (imports them for metadata) into the shared pool —
// it does NOT activate them. Activate the project's chosen plugins with EnablePlugin().
NUKEENGINE_API void InitModules(AppInstance* instance);
NUKEENGINE_API void UnloadModules();

// Activate / deactivate a discovered plugin at any time (the manager's checkboxes + the
// per-project load list drive these). Enable = OnLoad() (sync, registers types) + Run()
// (background). Disable = Shutdown(). Idempotent.
NUKEENGINE_API void EnablePlugin(NUKEModule* m);
NUKEENGINE_API void DisablePlugin(NUKEModule* m);

// Which plugin (dll name) provides a component type, "" for engine built-ins. Learned by
// diffing the reflection registry around each plugin's OnLoad().
NUKEENGINE_API const char* PluginForType(const std::string& type);

// True if a type's components should be live (built-in, or its plugin is currently loaded);
// false means load them as inert UnknownComponent placeholders.
NUKEENGINE_API bool IsTypeActive(const std::string& type);

}  // namespace nuke

#endif // !NUKE_MODULAR_H

