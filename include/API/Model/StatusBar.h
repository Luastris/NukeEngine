#pragma once
#ifndef NUKEE_STATUSBAR_H
#define NUKEE_STATUSBAR_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// Editor status-bar fields: the editor renders its built-in stats and then every field set
// here, in first-set order. A field with a PROGRESS value renders as a background JOB (progress
// bar + jobs drop-up entry). Thread-safe: any thread may Set.
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

	// Create or update a field; `key` is the stable identity and the order slot. The 2-arg form
	// is a plain text field (progress reset to kNoProgress).
	static void Set(const std::string& key, const std::string& text);
	static void Set(const std::string& key, const std::string& text, float progress);
	static void Remove(const std::string& key);   // drop a field
	static std::vector<Entry> All();              // ordered snapshot for the UI
};

}  // namespace nuke

#endif // !NUKEE_STATUSBAR_H
