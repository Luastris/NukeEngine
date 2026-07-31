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
