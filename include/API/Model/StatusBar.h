#pragma once
#ifndef NUKEE_STATUSBAR_H
#define NUKEE_STATUSBAR_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// Editor status-bar fields (roadmap 2.3). The EDITOR renders the built-in stats
// (fps / frame time / backend / atoms / draws / memory) and then every field set
// here, in first-set order. Plugins use this to surface their own live status
// ("Jolt: 128 bodies", "Net: connected", ...). Thread-safe: any thread may Set.
//
// A field with a PROGRESS value is a background JOB: the bar shows it with a
// progress bar and it appears in the jobs drop-up list (async import etc.).
class NUKEENGINE_API StatusBar
{
public:
	static constexpr float kNoProgress    = -1.0f;   // plain text field (default)
	static constexpr float kIndeterminate = -2.0f;   // job of unknown length (animated bar)

	struct Entry
	{
		std::string key, text;
		float progress = kNoProgress;   // kNoProgress | kIndeterminate | [0..1]
		bool IsJob() const { return progress >= 0.0f || progress == kIndeterminate; }
	};

	// Create or update a field. The key is the stable identity (and the order slot);
	// the text is what the bar shows. Empty text keeps the field but shows nothing.
	// The 2-arg form is a plain text field (progress reset to kNoProgress).
	static void Set(const std::string& key, const std::string& text);
	static void Set(const std::string& key, const std::string& text, float progress);
	// Drop a field (job finished / the plugin is shutting down).
	static void Remove(const std::string& key);
	// Ordered snapshot for the UI.
	static std::vector<Entry> All();
};

}  // namespace nuke

#endif // !NUKEE_STATUSBAR_H
