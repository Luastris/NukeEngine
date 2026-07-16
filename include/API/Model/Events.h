#pragma once
#ifndef NUKEE_EVENTS_H
#define NUKEE_EVENTS_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>
#include <functional>

namespace nuke {

// Engine-wide EVENT BUS + game-time scheduler (colony-sim base, Phase 6.3) — the
// cross-script/cross-system messaging seam (Lua ↔ C# ↔ native game modules).
//
// Model:
//  - Emit(name, payload) queues an event (thread-safe — collision handlers on the fixed
//    thread may emit). The queue drains ONCE per frame in World::Update under the game
//    lock: first to native subscribers (C++ game systems), then to every enabled
//    component's OnEvent(name, payload) hook — ScriptComponent forwards it to the Lua
//    `onEvent(self, name, payload)` function, CSharpScript to a C# `OnEvent(string,string)`
//    method, native components just override the virtual.
//  - The SCHEDULER rides the game calendar (Time.TotalGameSeconds — speed-scaled, frozen
//    at pause): After() fires once, Every() repeats. Pending entries SAVE WITH THE WORLD
//    (a savegame resumes its pending incidents); subscriptions are runtime-only.
//  - Payload is a plain string (JSON by convention when structure is needed).
// Engine time events emitted automatically: "time.newHour", "time.newDay", "time.newMonth".
class NUKEENGINE_API Events
{
	NUKE_CLASS_NOCREATE(Events, Object)
public:
	// Reflected script surface (auto-bound as nuke.Events.* / C# Events.*).
	[[nuke::func]] static void   Emit(const std::string& name, const std::string& payload);
	// Fire once after `gameSeconds` of GAME time (calendar seconds — 60 = one in-game
	// minute; speed-scaled, frozen while paused). Returns a cancel id.
	[[nuke::func]] static double After(double gameSeconds, const std::string& name, const std::string& payload);
	// Fire repeatedly every `gameSeconds` of game time. Returns a cancel id.
	[[nuke::func]] static double Every(double gameSeconds, const std::string& name, const std::string& payload);
	[[nuke::func]] static void   Cancel(double id);
	[[nuke::func]] static int    PendingCount();   // scheduled entries alive

	// --- native (C++ game modules / engine systems; not marshaled to scripts) ---
	using Handler = std::function<void(const std::string& name, const std::string& payload)>;
	// Subscribe a native handler to an exact event name ("" = every event). Returns an id.
	static long long Subscribe(const std::string& name, Handler fn);
	static void      Unsubscribe(long long id);
	// Emit from engine internals (identical to Emit; explicit name for call-site clarity).
	static void      EmitEngine(const std::string& name, const std::string& payload);

	// --- host plumbing (World owns the lifecycle) ---
	// Drain queued events + fire due scheduled ones. Called by World::Update under the
	// game lock; `dispatch` delivers each event to the world's components.
	static void Pump(const std::function<void(const std::string&, const std::string&)>& dispatch);
	static std::string SaveJson();               // pending schedule -> JSON (world save)
	static void        LoadJson(const std::string& js);   // restore (world load)
	static void        ResetSchedule();          // world switch without a saved schedule
};

}  // namespace nuke

#endif // !NUKEE_EVENTS_H
