#pragma once
#ifndef NUKEE_MESH_H
#define NUKEE_MESH_H
#include "NukeAPI.h"
#include <istream>
#include "Transform.h"
#include "Material.h"
#include <assimp/mesh.h>
#include <boost/container/list.hpp>
#include <memory>
#include <string>
#include <vector>
#include "../../../NukeEngine.h"
#include "reflect/Reflect.h"   // NUKE_CLASS (reflected asset)

struct aiScene;   // fwd (skin import needs the node hierarchy)

namespace nuke {

namespace bc = boost::container;
//class Mesh;
//template class bc::list<Mesh*>;

// One skeleton joint (animation, roadmap 3.1). Bones are stored in HIERARCHY ORDER
// (parent index always < own index), so a single forward pass computes globals.
// Non-weighted intermediate nodes on the root->bone paths are included too (clips may
// animate them); their invBind stays identity and no vertex references them.
struct MeshBone
{
	std::string name;                                   // matches AnimClip channel names
	int   parent = -1;                                  // index into Mesh::bones; -1 = root
	float invBind[16];                                  // inverse bind (offset) matrix, glm column-major
	// Local bind pose, decomposed (the rest pose for bones a clip doesn't animate;
	// TRS so poses BLEND without runtime matrix decomposition).
	float localPos[3]   = { 0, 0, 0 };
	float localRot[4]   = { 0, 0, 0, 1 };               // (x, y, z, w)
	float localScale[3] = { 1, 1, 1 };
};

class NUKEENGINE_API Mesh
{
    // Reflected ASSET class (scripts create/edit/assign it like any engine object).
    NUKE_CLASS(Mesh, Object)
public:
    char name[256];
    std::string guid;   // asset id ("builtin:cube" for primitives, generated for imports)
    float *vertexArray;
    float *normalArray;
    float *uvArray;

    int numVerts;

	// --- skinning (roadmap 3.1; empty on rigid meshes) --------------------------------
	// Per-vertex bone bindings, 4 per vertex (assimp LimitBoneWeights caps at 4): indices
	// into `bones`, weights normalized to sum 1. Unindexed like the other arrays.
	// u16: FBX rigs (mixamo) grow $AssimpFbx$ helper nodes — skeletons pass 255 joints.
	unsigned short* boneIndex  = nullptr;   // 4 * numVerts
	float*          boneWeight = nullptr;   // 4 * numVerts
	std::vector<MeshBone> bones;
	bool HasSkin() const { return boneIndex && boneWeight && !bones.empty(); }

	// Data generation: bump after changing vertex/normal data in place (skinned instances,
	// procedural meshes) — the renderer re-uploads its cached GPU buffers on mismatch.
	int version = 0;

	// Ray-tracing stand-in for DYNAMIC meshes: BLAS/TLAS use this mesh instead (a skinned
	// instance points at its bind-pose source — per-frame BLAS rebuilds are a non-goal).
	Mesh* rtProxy = nullptr;

	// RT WIND BEND (7.4, merged foliage chunks only): when present, the renderer runs the
	// NukeBend compute over this mesh every frame and REFITS its BLAS — ray-traced shadows
	// and reflections of the vegetation sway exactly like the raster blades.
	// rtBendArray  = 4 floats/vert: (source-mesh local Y, custom.z windGate, custom.w interactGate, 0)
	// rtPivotArray = 4 floats/vert: (instance origin in atom space, 0) — the bend phase hash.
	float* rtBendArray  = nullptr;
	float* rtPivotArray = nullptr;
	// RT DYNAMIC mesh (particle quads): the vertex data is rewritten EVERY frame (version
	// bump -> in-place buffer update) and the renderer REBUILDS the cached BLAS over it each
	// frame (scratch kept, capacity constant — dead space collapses to degenerate triangles).
	bool rtDynamic = false;
	// Non-opaque RT geometry: rays run the ALPHA-TEST any-hit (albedo-map alpha) instead of
	// committing on the whole surface — cutout sprites in reflections; shadow rays treat each
	// quad by its rtShadowShape footprint (world.ps candidate loop).
	bool rtAlphaTested = false;
	// PER-VERTEX RGBA (4 floats/vert) for RT shading of dynamic sprite meshes: particle
	// gradient/fade colors. Re-read EVERY FRAME into the renderer's dynamic color pool
	// (rtDynamic meshes only) — reflections tint and fade exactly like the direct view.
	float* rtColorArray = nullptr;
	// Shadow-ray footprint of NON-OPAQUE quads (world.ps candidate loop, no textures there):
	// 0 = full quad, 1 = disc (round sprites), 2 = strip across u (trail ribbons).
	int rtShadowShape = 0;

	// Local-space bounds (for frustum culling). Lazily computed from vertexArray on first use.
	float aabbMin[3] = { 0, 0, 0 };
	float aabbMax[3] = { 0, 0, 0 };
	bool  boundsValid = false;
	void  EnsureBounds();

	bc::list<Mesh*>  children;


	Mesh();

	// `scene` (optional) enables skin import: bone weights + the skeleton from the node tree.
	void ImportAIMesh(aiMesh* mesh, const aiScene* scene = nullptr);

	// Primitive factories (procedural geometry). Registered in ResDB under "builtin:<name>".
	static Mesh* CreateCube();
	static Mesh* CreatePlane();
	static Mesh* CreateSphere();
	static Mesh* CreateCylinder();
	static Mesh* CreateCapsule();
	// Built-in foliage placeholders (7.4): honest geometry, no textures needed — the scatter
	// works out of the box; swap in imported meshes for beauty. Registered as
	// builtin:grassclump / builtin:bush / builtin:tree in ResDB.
	static Mesh* CreateGrassClump();
	static Mesh* CreateBush();
	static Mesh* CreateTree();

	// Native asset format (.numesh): binary header + interleaved-free vertex/normal/uv arrays.
	// Import converts external files (OBJ/FBX/...) into these so nothing references the source
	// at runtime. The GUID is stored inside the file; ResDB indexes by it.
	bool         SaveToFile(const std::string& path) const;
	static Mesh* LoadFromFile(const std::string& path);
	static Mesh* LoadFromMemory(const std::string& data);   // packed content (3.2) — no disk copy
	static Mesh* LoadFromStream(std::istream& i);
};
}  // namespace nuke

#endif // !NUKEE_MESH_H
