#pragma once
#ifndef NUKEE_BENDVOLUMES_H
#define NUKEE_BENDVOLUMES_H
#include "NukeAPI.h"
#include <vector>

namespace nuke {

// A per-frame ANALYTIC VOLUME that bends foliage in the instanced vertex shaders (7.4):
// wind zones, VFX force fields, future weather/explosions — anything localized. The engine
// pushes up to 16 of these (nearest to the camera) into the renderer's BendCB every frame.
//
// mode: 0 = directional (dir * strength), 1 = radial (outward from pos; negative strength
// pulls inward), 2 = vortex (swirl around Y at pos), 3 = turbulence (animated noise jitter).
// falloff: 0 = full strength to the edge, 1 = linear fade to the edge. strength in m/s-ish
// wind units (the shader scales it exactly like the global wind).
struct BendVolume
{
	float pos[3] = { 0, 0, 0 };
	float radius = 1.0f;
	float dir[3] = { 0, 0, 1 };
	float strength = 0.0f;
	int   mode = 0;
	float falloff = 1.0f;
};

// Frame accumulator for MODULE-side volumes (engine systems like WindZone are collected
// directly). Submit from anywhere during the frame (thread-safe); World::Render consumes
// the batch when it builds the BendCB — submissions made during rendering land next frame
// (1-frame latency, order-independent).
class NUKEENGINE_API BendVolumes
{
public:
	static void Submit(const BendVolume& v);
	static std::vector<BendVolume> Consume();
};

}  // namespace nuke

#endif // !NUKEE_BENDVOLUMES_H
