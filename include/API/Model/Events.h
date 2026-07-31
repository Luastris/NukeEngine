#pragma once
#ifndef NUKEE_EVENTS_H
#define NUKEE_EVENTS_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>
#include <functional>

namespace nuke {

// Engine-wide event bus + game-time scheduler (Lua / C# / native). Emit is thread-safe; the queue
// drains once per frame in World::Update. Scheduled entries save with the world, subscriptions do not.
// Engine time events emitted automatically: "time.newHour", "time.newDay", "time.newMonth".
class NUKEENGINE_API Events
{
	NUKE_CLASS_NOCREATE(Events, Object)
public:
	[[nuke::func]] static void   Emit(const std::string& name, const std::string& payload);
	// Fire once after `gameSeconds` of GAME time (speed-scaled, frozen while paused). Returns a cancel id.
	[[nuke::func]] static double After(double gameSeconds, const std::string& name, const std::string& payload);
	// Fire repeatedly every `gameSeconds` of game time. Returns a cancel id.
	[[nuke::func]] static double Every(double gameSeconds, const std::string& name, const std::string& payload);
	[[nuke::func]] static void   Cancel(double id);
	[[nuke::func]] static int    PendingCount();   // scheduled entries alive

	// --- native (not marshaled to scripts) ---
	using Handler = std::function<void(const std::string& name, const std::string& payload)>;
	// Subscribe a native handler to an exact event name ("" = every event). Returns an id.
	static long long Subscribe(const std::string& name, Handler fn);
	static void      Unsubscribe(long long id);
	static void      EmitEngine(const std::string& name, const std::string& payload);

	// --- host plumbing (World owns the lifecycle) ---
	// Drain queued events + fire due scheduled ones; `dispatch` delivers each to the world's components.
	static void Pump(const std::function<void(const std::string&, const std::string&)>& dispatch);
	static std::string SaveJson();               // pending schedule -> JSON (world save)
	static void        LoadJson(const std::string& js);   // restore (world load)
	static void        ResetSchedule();          // world switch without a saved schedule
};

}  // namespace nuke

#endif // !NUKEE_EVENTS_H
