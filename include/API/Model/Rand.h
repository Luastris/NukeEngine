#pragma once
#ifndef NUKEE_RAND_H
#define NUKEE_RAND_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

// Deterministic seeded RNG with NAMED STREAMS: each stream ("mapgen", "storyteller", ...) is an
// independent PCG32 sequence, so its draws replay identically whatever the other streams consume.
// The unnamed stream ("") is the default. State/SetState carry a stream across save/load.
class NUKEENGINE_API Rand
{
	NUKE_CLASS_NOCREATE(Rand, Object)
public:
	// Seed a stream. Same seed = same sequence on every machine.
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

	// Native fast path (no string lookup per draw): resolve the stream once, then draw. The
	// handle stays valid for the session.
	static void*    StreamHandle(const std::string& stream);
	static uint32_t Next(void* streamHandle);                    // raw 32 uniform bits
	static double   ValueH(void* streamHandle);                  // [0,1)
	static int      RangeIntH(void* streamHandle, int minv, int maxv);
};

}  // namespace nuke

#endif // !NUKEE_RAND_H
