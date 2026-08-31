#pragma once
#ifndef NUKEE_PROFILER_H
#define NUKEE_PROFILER_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

// Per-phase frame timings in EMA-smoothed milliseconds. The engine reports "update", "fixed"
// and "render" every frame; game modules report their own phases via Report/Scope.
class NUKEENGINE_API Profiler
{
	NUKE_CLASS_NOCREATE(Profiler, Object)
public:
	// Smoothed milliseconds of a phase this frame (0 = unknown phase / not reported yet).
	[[nuke::func]] static double      Ms(const std::string& phase);
	[[nuke::func]] static std::string Phases();   // newline-separated known phase names
	// Snapshot every phase to a CSV ("phase;ms", heaviest first). Relative paths land next
	// to the executable's working directory. Returns false on IO failure.
	[[nuke::func]] static bool        Capture(const std::string& file);

	// --- perf overlays (engine-drawn, top-right of the game screen) ---
	// FPS counter and frame-time graph over the running game — player fullscreen included.
	// Toggled from the dev console ("Profiler.ShowFps true") or any script; both enabled
	// stack vertically, never overlapping. Hidden while the dev console is open.
	[[nuke::func]] static void ShowFps(bool on);
	[[nuke::func]] static void ShowGraph(bool on);
	[[nuke::func]] static bool FpsShown();
	[[nuke::func]] static bool GraphShown();

	// Engine-internal: sample this frame into the history ring and draw the enabled overlays
	// through the GUI backend seam. Called once per GUI frame by Ui::EmitFrame.
	static void EmitOverlay();

	// --- native reporting (engine internals + game modules) ---
	static void Report(const std::string& phase, double ms);   // thread-safe

	// Scoped helper: Profiler::Scope s("pawns"); — reports on destruction.
	struct NUKEENGINE_API Scope
	{
		explicit Scope(const char* phase);
		~Scope();
	private:
		const char* name;
		double      t0;
	};
};

}  // namespace nuke

#endif // !NUKEE_PROFILER_H
