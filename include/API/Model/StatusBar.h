#pragma once
#ifndef NUKEE_STATUSBAR_H
#define NUKEE_STATUSBAR_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include <utility>

namespace nuke {

// Editor status-bar fields (roadmap 2.3). The EDITOR renders the built-in stats
// (fps / frame time / backend / atoms / draws / memory) and then every field set
// here, in first-set order. Plugins use this to surface their own live status
// ("Jolt: 128 bodies", "Net: connected", ...). Thread-safe: any thread may Set.
class NUKEENGINE_API StatusBar
{
public:
	// Create or update a field. The key is the stable identity (and the order slot);
	// the text is what the bar shows. Empty text keeps the field but shows nothing.
	static void Set(const std::string& key, const std::string& text);
	// Drop a field (e.g. the plugin is shutting down).
	static void Remove(const std::string& key);
	// Ordered snapshot for the UI.
	static std::vector<std::pair<std::string, std::string>> All();
};

}  // namespace nuke

#endif // !NUKEE_STATUSBAR_H
