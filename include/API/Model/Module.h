#pragma once
#ifndef NUKEE_MODULE_H
#define NUKEE_MODULE_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// Read-only public view of the plugin pool: metadata snapshots for game/tool code
// ("is scripting on?", an about box, diagnostics). Deliberately NOT the loader —
// enable/disable lives host-side (interface/Modular.h + the editor's plugin window);
// gameplay must not toggle plugins.
class NUKEENGINE_API Module
{
public:
	std::string title;
	std::string version;
	std::string author;
	std::string site;
	std::string file;       // dll file name — the stable id (project load lists use it)
	std::string provides;   // service label ("render"/"scripting"/"gui"/...; "" = utility)
	std::vector<std::string> tags;
	bool loaded = false;    // currently active
	int  phase  = 0;        // PluginPhase (PHASE_BOOT / PHASE_RUNTIME)

	// Snapshot of every DISCOVERED plugin (loaded or not), in discovery order.
	static std::vector<Module> All();
	// True when the plugin with this dll name (or title) is currently active.
	static bool IsLoaded(const std::string& fileOrTitle);
};

}  // namespace nuke

#endif // !NUKEE_MODULE_H
