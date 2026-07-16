#pragma once
#ifndef NUKEE_RAND_H
#define NUKEE_RAND_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

// Deterministic seeded RNG with NAMED STREAMS (colony-sim base, Phase 6.2).
//
// Each stream ("mapgen", "storyteller", "combat", ...) is an independent PCG32 sequence:
// seed a stream once and its draws replay identically regardless of what the other streams
// consume — mapgen stays reproducible while the storyteller rolls freely. The unnamed
// stream ("") is the convenience default. Reflected → nuke.Rand.* / C# Rand.* 1:1; native
// game modules call the same statics directly.
//
// Determinism across save/load: streams expose their raw state (State/SetState) — savegame
// code stores the streams it cares about (6.6 wires this into the savegame automatically).
class NUKEENGINE_API Rand
{
	NUKE_CLASS_NOCREATE(Rand, Object)
public:
	// Seed a stream. Same seed = same sequence, forever, on every machine.
	[[nuke::func]] static void   Seed(const std::string& stream, double seed);
	// Uniform double in [0, 1).
	[[nuke::func]] static double Value(const std::string& stream);
	// Uniform double in [min, max).
	[[nuke::func]] static double Range(const std::string& stream, double minv, double maxv);
	// Uniform int in [min, max] (inclusive).
	[[nuke::func]] static int    RangeInt(const std::string& stream, int minv, int maxv);
	// True with probability p (0..1).
	[[nuke::func]] static bool   Chance(const std::string& stream, double p);
	// Normal distribution (Box-Muller) with the given mean / standard deviation.
	[[nuke::func]] static double Gauss(const std::string& stream, double mean, double dev);
	// Raw stream state for savegames: capture with State, restore with SetState.
	[[nuke::func]] static double State(const std::string& stream);
	[[nuke::func]] static void   SetState(const std::string& stream, double state);

	// Native fast path (no string lookup per draw): resolve the stream once, then draw.
	// The returned handle stays valid for the session (streams are never removed).
	static void*    StreamHandle(const std::string& stream);
	static uint32_t Next(void* streamHandle);                    // raw 32 uniform bits
	static double   ValueH(void* streamHandle);                  // [0,1)
	static int      RangeIntH(void* streamHandle, int minv, int maxv);
};

}  // namespace nuke

#endif // !NUKEE_RAND_H
