#pragma once
#ifndef NUKEE_NOISE_H
#define NUKEE_NOISE_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

// Coherent-noise toolkit for procedural generation (colony-sim base, Phase 6.2).
//
// All functions are STATELESS and fully deterministic: (seed, coordinates) → value, same
// on every machine — a map generated from a seed replays exactly. Reflected → nuke.Noise.*
// / C# Noise.* for scripts; native game modules call the statics directly on hot paths
// (a 250×250 heightmap is 62.5k calls — trivial in C++).
//
// Ranges: Perlin2/Perlin3 return [-1, 1]; Fbm returns roughly [-1, 1] (normalized by the
// octave weights); Voronoi2 returns the F1 distance (0 at a feature point, ~1 mid-cell).
class NUKEENGINE_API Noise
{
	NUKE_CLASS_NOCREATE(Noise, Object)
public:
	// Improved Perlin gradient noise (seed-hashed permutation).
	[[nuke::func]] static double Perlin2(double seed, double x, double y);
	[[nuke::func]] static double Perlin3(double seed, double x, double y, double z);
	// Fractal Brownian motion over Perlin2: `octaves` layers, each `lacunarity`× the
	// frequency and `gain`× the amplitude of the previous (classic terrain: 4, 2.0, 0.5).
	[[nuke::func]] static double Fbm(double seed, double x, double y,
	                                 int octaves, double lacunarity, double gain);
	// Cellular/Voronoi F1: distance to the nearest jittered feature point (ore blobs,
	// biome patches). CellId returns a stable id of the OWNING cell for region labeling.
	[[nuke::func]] static double Voronoi2(double seed, double x, double y);
	[[nuke::func]] static double CellId2(double seed, double x, double y);
	// Domain warp helper: Perlin-offset the sample position before sampling something else
	// (breaks up the grid look). Returns x/y warped by `amp` — call per axis.
	[[nuke::func]] static double WarpX(double seed, double x, double y, double amp);
	[[nuke::func]] static double WarpY(double seed, double x, double y, double amp);
};

}  // namespace nuke

#endif // !NUKEE_NOISE_H
