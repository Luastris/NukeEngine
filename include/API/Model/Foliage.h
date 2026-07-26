#pragma once
#ifndef NUKEE_FOLIAGE_H
#define NUKEE_FOLIAGE_H
#include "API/Model/InstancedMesh.h"

namespace nuke {

// Foliage (7.4): a PERSISTENT instanced scatter layer over any mesh surface. Derives from
// InstancedMesh, so the whole render integration comes inherited: chunked instance buffers,
// per-chunk frustum culling in every pass (lit / shadow / gbuffer-velocity / RT), the
// base64 instance blob, hot-applied mesh/material assets. On top it adds:
//   - procedural scatter RULES: density + seed over AREA-WEIGHTED triangles of a surface
//     subtree, masked by slope / world-height band / Perlin patch noise;
//   - WIND bend (vertex shader): per-instance coefficient + the global nuke::Wind params,
//     packed into instance custom.z (custom.w = interaction coefficient);
//   - INTERACTION bend: characters/dynamic bodies become "pushers" the shader bends
//     blades away from (visual, not physics);
//   - editor paint/fill: Rebuild() = fill by rules, PaintAt/EraseAt = brush strokes.
// One component = one layer (one mesh+material+rules); several layers = several
// components. Instances live in component DATA — never thousands of atoms.
class NUKEENGINE_API Foliage : public InstancedMesh
{
	NUKE_CLASS(Foliage, InstancedMesh, "World")
public:
	// ---- scatter ---------------------------------------------------------------------------
	[[nuke::prop(label="Surface", tip="Atom (with its children) whose meshes to scatter over. Empty = this atom's own meshes.")]] Atom* surface = nullptr;
	[[nuke::prop(label="Density", min=0, tip="Instances per square meter of surface area.")]] float density = 4.0f;
	[[nuke::prop(label="Seed", min=0, tip="Same seed + same rules = the same scatter.")]] int seed = 1337;
	[[nuke::prop(label="Scale Min", min=0.01)]] float scaleMin = 0.8f;
	[[nuke::prop(label="Scale Max", min=0.01)]] float scaleMax = 1.3f;
	[[nuke::prop(label="Random Yaw", tip="Random rotation around the growth axis.")]] bool randomYaw = true;
	[[nuke::prop(label="Align To Normal", min=0, max=1, tip="0 = grows straight up, 1 = follows the surface normal.")]] float alignToNormal = 1.0f;
	[[nuke::prop(label="Surface Offset", tip="Lift (positive) or sink along the surface normal, world units.")]] float surfaceOffset = 0.0f;
	// ---- masks -----------------------------------------------------------------------------
	[[nuke::prop(label="Max Slope", min=0, max=90, tip="Degrees from horizontal; steeper spots grow nothing.")]] float maxSlope = 45.0f;
	[[nuke::prop(label="Height Min", tip="World-Y band; spots outside are masked out.")]] float heightMin = -100000.0f;
	[[nuke::prop(label="Height Max")]] float heightMax = 100000.0f;
	[[nuke::prop(label="Noise Scale", min=0.1, tip="World units per patch-noise feature.")]] float noiseScale = 8.0f;
	[[nuke::prop(label="Noise Cover", min=0, max=1, tip="1 = everywhere, 0.5 = noisy patches over half the area, 0 = nothing.")]] float noiseCover = 1.0f;
	// ---- motion ----------------------------------------------------------------------------
	[[nuke::prop(label="Wind Bend", min=0, max=2, tip="How much the global wind bends this layer (top of the mesh sways, base stays).")]] float windBend = 1.0f;
	[[nuke::prop(label="Interaction Bend", min=0, max=2, tip="How much characters and moving bodies part this layer.")]] float interactionBend = 1.0f;

	// ---- reflected ops (the editor's Fill/Paint tools drive these; scripts can too) --------
	[[nuke::func]] void Rebuild();                                              // full re-scatter (Fill)
	[[nuke::func]] void PaintAt(const Vector3& worldPos, double radius, double densityMul);
	[[nuke::func]] void EraseAt(const Vector3& worldPos, double radius);

	Foliage();
	bool EnsureRenderReady(iRender* r) override;   // sync per-instance bend coefs, then base
	// The layer follows the atom's position/rotation but NOT its scale: foliage often sits
	// directly on a stretched ground atom (scale 40x1x40 city plane) — multiplying that into
	// every blade turned each one into a sheet bigger than the map.
	bool ScaleWithAtom() const override { return false; }

	// runtime: bend coefs the instances were last written with (prop edits hot-apply)
	float windBendRes = -1.0f, interBendRes = -1.0f, meshHeightRes = -1.0f;

private:
	// Scatter over the surface subtree. brushR <= 0 = whole surface (Fill); otherwise only
	// points within brushR of brushPos (world) are considered (Paint).
	void Scatter(const Vector3& brushPos, float brushR, float densMul);
	Atom* SurfaceRoot();
	float MeshHeight();                          // resolved mesh local AABB height (bend norm)
	void  SyncBendParams(bool force);
};

}  // namespace nuke

#endif // !NUKEE_FOLIAGE_H
