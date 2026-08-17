#pragma once
#ifndef NUKE_SCATTERSOURCES_H
#define NUKE_SCATTERSOURCES_H
// Generic scatter-surface providers: a module owning renderable ground the engine cannot see
// through MeshRenderers (voxel terrain nodes) registers one, and Foliage::Scatter grows over
// its triangles — including the LiveMaterial auto-foliage the Surface pump drives.
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

class Atom;

// One offered triangle: WORLD-space corners + a 0..1 density gate (splat layer weight).
struct ScatterTri
{
	float a[3], b[3], c[3];
	float weight = 1.0f;
};

// Provider contract. Collect returns false when the atom is not this source's; matGuid narrows
// to surface areas carrying that material ("" = the whole surface, weight 1).
struct ScatterSource
{
	virtual ~ScatterSource() {}
	virtual bool Collect(Atom* atom, const std::string& matGuid, std::vector<ScatterTri>& out) = 0;
};

NUKEENGINE_API void RegisterScatterSource(ScatterSource* s);
NUKEENGINE_API void UnregisterScatterSource(ScatterSource* s);
NUKEENGINE_API const std::vector<ScatterSource*>& ScatterSourceList();

}  // namespace nuke

#endif // !NUKE_SCATTERSOURCES_H
