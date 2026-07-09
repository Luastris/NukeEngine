#pragma once
#ifndef NUKEE_MESH_H
#define NUKEE_MESH_H
#include "NukeAPI.h"
#include "Transform.h"
#include "Material.h"
#include <assimp/mesh.h>
#include <boost/container/list.hpp>
#include <memory>
#include <string>
#include <vector>
#include "../../../NukeEngine.h"

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

	// Native asset format (.numesh): binary header + interleaved-free vertex/normal/uv arrays.
	// Import converts external files (OBJ/FBX/...) into these so nothing references the source
	// at runtime. The GUID is stored inside the file; ResDB indexes by it.
	bool         SaveToFile(const std::string& path) const;
	static Mesh* LoadFromFile(const std::string& path);
};
}  // namespace nuke

#endif // !NUKEE_MESH_H
