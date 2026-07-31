#pragma once
#ifndef NUKEE_NOISE_H
#define NUKEE_NOISE_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

// Coherent-noise toolkit for procedural generation. All functions are stateless and deterministic:
// (seed, coordinates) → value, identical on every machine.
// Ranges: Perlin2/Perlin3 [-1, 1]; Fbm roughly [-1, 1]; Voronoi2 = F1 distance (0 at a feature point).
class NUKEENGINE_API Noise
{
	NUKE_CLASS_NOCREATE(Noise, Object)
public:
	// Improved Perlin gradient noise (seed-hashed permutation).
	[[nuke::func]] static double Perlin2(double seed, double x, double y);
	[[nuke::func]] static double Perlin3(double seed, double x, double y, double z);
	// Fractal Brownian motion over Perlin2: `octaves` layers, each `lacunarity`x the frequency
	// and `gain`x the amplitude of the previous (classic terrain: 4, 2.0, 0.5).
	[[nuke::func]] static double Fbm(double seed, double x, double y,
	                                 int octaves, double lacunarity, double gain);
	// Cellular/Voronoi F1: distance to the nearest jittered feature point.
	// CellId returns a stable id of the owning cell for region labeling.
	[[nuke::func]] static double Voronoi2(double seed, double x, double y);
	[[nuke::func]] static double CellId2(double seed, double x, double y);
	// Domain warp: returns the sample position offset by `amp` — call per axis.
	[[nuke::func]] static double WarpX(double seed, double x, double y, double amp);
	[[nuke::func]] static double WarpY(double seed, double x, double y, double amp);
};

}  // namespace nuke

#endif // !NUKEE_NOISE_H
