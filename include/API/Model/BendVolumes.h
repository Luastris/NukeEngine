#pragma once
#ifndef NUKEE_BENDVOLUMES_H
#define NUKEE_BENDVOLUMES_H
#include "NukeAPI.h"
#include <vector>

namespace nuke {

// A per-frame analytic volume that bends foliage in the instanced vertex shaders (wind zones,
// force fields, explosions). Up to 16 nearest to the camera go into the renderer's BendCB.
struct BendVolume
{
	float pos[3] = { 0, 0, 0 };
	float radius = 1.0f;
	float dir[3] = { 0, 0, 1 };
	float strength = 0.0f;   // wind units; the shader scales it like the global wind
	int   mode = 0;          // 0 directional, 1 radial (negative pulls in), 2 vortex, 3 turbulence
	float falloff = 1.0f;    // 0 = full strength to the edge, 1 = linear fade
	float pull = 0.0f;       // vortex: +1 flows out of the centre (diverges), -1 flows into it (converges), 0 turns in place
	// vortex funnel (the fog's dents): inner radius (0 = 15% of the radius), depth 0..1, sharpness 0..1, size m (0 = the fog's noise scale), extra density in the folds
	float inner = 0.0f, dentDepth = 0.7f, dentSharp = 0.5f, dentSize = 0.0f, dentDensity = 0.5f;
};

// Frame accumulator for MODULE-side volumes (thread-safe). World::Render consumes the batch
// when it builds the BendCB — submissions made during rendering land next frame.
class NUKEENGINE_API BendVolumes
{
public:
	static void Submit(const BendVolume& v);
	static std::vector<BendVolume> Consume();
};

}  // namespace nuke

#endif // !NUKEE_BENDVOLUMES_H
