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

// One skeleton joint. Bones MUST stay in hierarchy order (parent index < own index) so a single
// forward pass computes globals. Unweighted intermediate nodes are included with identity invBind.
struct MeshBone
{
	std::string name;                                   // matches AnimClip channel names
	int   parent = -1;                                  // index into Mesh::bones; -1 = root
	float invBind[16];                                  // inverse bind (offset) matrix, glm column-major
	// Local bind pose, decomposed to TRS so poses blend without runtime matrix decomposition.
	float localPos[3]   = { 0, 0, 0 };
	float localRot[4]   = { 0, 0, 0, 1 };               // (x, y, z, w)
	float localScale[3] = { 1, 1, 1 };
};

class NUKEENGINE_API Mesh
{
    NUKE_CLASS(Mesh, Object)
public:
    char name[256];
    std::string guid;   // asset id ("builtin:cube" for primitives, generated for imports)
    float *vertexArray;
    float *normalArray;
    float *uvArray;

    int numVerts;

	// --- skinning (empty on rigid meshes) ----
	// Per-vertex bone bindings, 4 per vertex: indices into `bones`, weights normalized to sum 1.
	// u16 indices: FBX rigs grow $AssimpFbx$ helper nodes, so skeletons pass 255 joints.
	unsigned short* boneIndex  = nullptr;   // 4 * numVerts
	float*          boneWeight = nullptr;   // 4 * numVerts
	std::vector<MeshBone> bones;
	bool HasSkin() const { return boneIndex && boneWeight && !bones.empty(); }

	int version = 0;   // bump after in-place vertex/normal edits; the renderer re-uploads on mismatch

	Mesh* rtProxy = nullptr;   // RT stand-in for dynamic meshes (a skinned instance points at its bind pose)

	// Per-frame RT wind bend (merged foliage chunks): the renderer runs the bend compute and refits the BLAS.
	// rtBendArray  = 4 floats/vert: (source-mesh local Y, custom.z windGate, custom.w interactGate, 0)
	// rtPivotArray = 4 floats/vert: (instance origin in atom space, 0) — the bend phase hash.
	float* rtBendArray  = nullptr;
	float* rtPivotArray = nullptr;
	bool rtDynamic = false;       // vertex data rewritten every frame; the cached BLAS is rebuilt each frame
	bool rtAlphaTested = false;   // rays run the alpha-test any-hit instead of committing on the whole surface
	float* rtColorArray = nullptr;   // per-vertex RGBA (4 floats/vert), re-read every frame for rtDynamic meshes
	// Shadow-ray footprint of non-opaque quads: 0 = full quad, 1 = disc, 2 = strip across u.
	int rtShadowShape = 0;

	// Local-space bounds for frustum culling; lazily computed from vertexArray.
	float aabbMin[3] = { 0, 0, 0 };
	float aabbMax[3] = { 0, 0, 0 };
	bool  boundsValid = false;
	void  EnsureBounds();

	bc::list<Mesh*>  children;


	Mesh();

	// `scene` (optional) enables skin import: bone weights + the skeleton from the node tree.
	void ImportAIMesh(aiMesh* mesh, const aiScene* scene = nullptr);

	// Primitive factories; registered in ResDB under "builtin:<name>".
	static Mesh* CreateCube();
	static Mesh* CreatePlane();
	static Mesh* CreateSphere();
	static Mesh* CreateCylinder();
	static Mesh* CreateCapsule();
	// Foliage placeholders, registered as builtin:grassclump / builtin:bush / builtin:tree.
	static Mesh* CreateGrassClump();
	static Mesh* CreateBush();
	static Mesh* CreateTree();

	// Native asset format (.numesh): binary header + separate vertex/normal/uv arrays.
	// The GUID is stored inside the file; ResDB indexes by it.
	bool         SaveToFile(const std::string& path) const;
	static Mesh* LoadFromFile(const std::string& path);
	static Mesh* LoadFromMemory(const std::string& data);   // packed content — no disk copy
	static Mesh* LoadFromStream(std::istream& i);
};
}  // namespace nuke

#endif // !NUKEE_MESH_H
