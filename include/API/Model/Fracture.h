#pragma once
#ifndef NUKEE_FRACTURE_H
#define NUKEE_FRACTURE_H
#include "NukeAPI.h"
#include "Vector.h"
#include <cstdint>
#include <vector>

namespace nuke {

class Mesh;

// One fracture fragment: an unindexed triangle soup in the SOURCE mesh's local frame,
// re-centered on `centroid` (spawn the piece atom at source pose + rotated centroid).
struct FracturePiece
{
	std::vector<float> verts;     // xyz per vertex, 3 verts per triangle
	std::vector<float> normals;
	std::vector<float> uvs;       // uv per vertex (cut faces get planar-projected coords)
	size_t surfVerts = 0;         // verts [0..surfVerts) = original surface; the rest = cut caps
	Vector3 centroid;
	Vector3 halfExtents;          // piece bounds (mass estimates)
};

// Runtime Voronoi fracture of the ACTUAL mesh: seeds split space into cells, the surface
// triangles are clipped into each cell and the cell's bisector faces cap the cuts (interior
// faces test against the mesh, so concave sources shed the fake caps). `scale` bakes the
// atom's world scale into the pieces. Returns the non-empty fragments.
NUKEENGINE_API bool FractureMesh(const Mesh* src, const Vector3& scale, int pieces,
                                 uint32_t seed, std::vector<FracturePiece>& out);

}  // namespace nuke

#endif // !NUKEE_FRACTURE_H
