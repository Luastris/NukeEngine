#pragma once
#ifndef NUKEE_STATUSBAR_H
#define NUKEE_STATUSBAR_H
#include "NukeAPI.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nuke {

// Editor status-bar fields. The bar has its built-in metrics and then ONE general-purpose
// message slot: of all text fields the one with the highest priority (newest on a tie) is shown
// there — the pipeline builder's "compiling: …", "Saved to …", module notes all compete for that
// single line instead of each claiming a slot. A field with a PROGRESS value is a background JOB
// (progress bar + jobs drop-up). Text fields expire on their own (ttl); jobs are removed
// explicitly. Thread-safe: any thread may Set.
class NUKEENGINE_API StatusBar
{
public:
	static constexpr float kNoProgress    = -1.0f;   // plain text field (default)
	static constexpr float kIndeterminate = -2.0f;   // job of unknown length (animated bar)

	struct Entry
	{
		std::string key, text;
		float    progress = kNoProgress;   // kNoProgress | kIndeterminate | [0..1]
		int      priority = 0;             // message slot: higher wins
		uint64_t seq = 0;                  // set order: newer wins among equal priorities
		double   expiresAt = 0.0;          // Log::Uptime seconds; 0 = never
		bool IsJob() const { return progress >= 0.0f || progress == kIndeterminate; }
	};

	// Create or update a field; `key` is the stable identity. The 2-arg form is a plain message
	// at priority 0 that expires after 10 s.
	static void Set(const std::string& key, const std::string& text);
	static void Set(const std::string& key, const std::string& text, float progress);   // job
	// A message for the single slot: `priority` decides who shows (background info ~10, user
	// actions like "Saved" ~50, problems ~90), `ttlSeconds` when it leaves on its own (0 = until
	// Remove / replaced).
	static void Message(const std::string& key, const std::string& text, int priority, double ttlSeconds);
	static void Remove(const std::string& key);   // drop a field
	static std::vector<Entry> All();              // ordered snapshot for the UI (expired ones dropped)
};

}  // namespace nuke

#endif // !NUKEE_STATUSBAR_H
