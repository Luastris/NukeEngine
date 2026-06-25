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
#include <boost/thread.hpp>

#ifdef USE_WINAPI
#include <Windows.h>
#endif // USE_WINAPI

namespace nuke {

// The loaded plugins live as a SINGLE instance inside the engine DLL. Access them via
// GetModules() — a header-level `static` would give every TU (e.g. the editor) its own
// empty copy, so the plugin manager would never see what InitModules loaded.
NUKEENGINE_API bc::vector<boost::shared_ptr<NUKEModule>>& GetModules();

NUKEENGINE_API void InitModules(AppInstance* instance);
NUKEENGINE_API void UnloadModules();

}  // namespace nuke

#endif // !NUKE_MODULAR_H

