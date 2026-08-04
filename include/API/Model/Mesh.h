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

// One material section: a contiguous index-buffer range drawn with material slot `slot`.
// Sections belonging to one LOD are contiguous, so a whole LOD is also one IB range.
struct MeshSection
{
	uint32_t firstIndex = 0;   // offset into Mesh::indexArray
	uint32_t indexCount = 0;
	int      slot       = 0;   // material slot (MeshRenderer::MaterialForSlot)
};

// One LOD level: a run of sections + the screen coverage below which the NEXT level takes over.
struct MeshLOD
{
	int   firstSection = 0;
	int   sectionCount = 0;
	float screenSize   = 0.0f;   // approx bounding-sphere diameter / viewport height; 0 = always
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

	// --- indexed geometry (v4; null on legacy/procedural soup meshes) ---------------------
	// Triangles are index triplets into the vertex streams. numIndices == 0 = unindexed soup
	// (numVerts/3 triangles) — builtins and procedural meshes (water/VFX/foliage) stay soup.
	uint32_t* indexArray = nullptr;
	int       numIndices = 0;

	// Optional per-vertex streams (v4; any may be null):
	float* tangentArray = nullptr;   // 4/vert: xyz tangent + w handedness (MikkTSpace convention)
	float* uv2Array     = nullptr;   // 2/vert: second UV set (lightmaps/detail)
	float* colorArray   = nullptr;   // 4/vert: vertex color RGBA

	// Skeleton ASSET reference (v5): the shared .nuskel this mesh's boneIndex stream points
	// into. Empty = legacy embedded skeleton (`bones` below) or a rigid mesh.
	std::string skelGuid;

	// Import-time material per SLOT (v7): the materials this mesh WAS AUTHORED with. The prefab
	// still owns what an instance renders with; this is the fallback every "just show me the
	// mesh" path needs — asset previews, the animation-editor rigs, dropping a bare mesh in.
	std::vector<std::string> defaultMats;

	// Morph targets / blend shapes (v6): DENSE per-vertex deltas against the bind streams,
	// applied before skinning (weights live on the SkinnedMeshRenderer).
	struct MorphTarget
	{
		std::string name;
		std::vector<float> posDelta;   // 3 * numVerts
		std::vector<float> nrmDelta;   // 3 * numVerts (may be empty — position-only target)
	};
	std::vector<MorphTarget> morphs;
	int MorphIndex(const std::string& morphName) const
	{
		for (size_t i = 0; i < morphs.size(); ++i)
			if (morphs[i].name == morphName) return (int)i;
		return -1;
	}

	// Material sections + LODs (v4). Empty sections = one implicit section over the whole
	// mesh, slot 0. Empty lods = one LOD over all sections.
	std::vector<MeshSection> sections;
	std::vector<MeshLOD>     lods;
	int numSlots = 1;                        // material slot count (== max section slot + 1)
	std::vector<std::string> slotNames;      // per-slot labels from import (inspector rows)

	// Index count of the LOD0 range — the mesh's REAL surface. LOD1+ simplified ranges follow
	// it in the IB and must never be consumed together with it (they are coincident shells).
	int Lod0IndexCount() const
	{
		if (numIndices <= 0) return numVerts;
		if (lods.empty() || sections.empty()) return numIndices;
		MeshLOD L = Lod(0);
		int n = 0;
		for (int s = 0; s < L.sectionCount; ++s) n += (int)Section(L.firstSection + s).indexCount;
		return n;
	}
	// Logical triangles = LOD0 only (soup meshes: the whole soup). Consumers sampling or
	// baking "the mesh's triangles" (foliage scatter, physics, VFX, stats) get exactly the
	// full-quality surface, never the appended LOD shells.
	int TriCount() const { return Lod0IndexCount() / 3; }
	// Vertex index of corner k (0..2) of triangle `tri` — works for soup AND indexed meshes.
	// Valid for tri < TriCount(): LOD0 starts at IB offset 0 by construction.
	uint32_t TriIndex(int tri, int k) const
	{ return numIndices > 0 ? indexArray[(size_t)tri * 3 + k] : (uint32_t)(tri * 3 + k); }
	// Effective section list: `sections` or the implicit whole-mesh one.
	int SectionCount() const { return sections.empty() ? 1 : (int)sections.size(); }
	MeshSection Section(int s) const
	{
		if (s >= 0 && s < (int)sections.size()) return sections[s];
		MeshSection all; all.firstIndex = 0;
		all.indexCount = (uint32_t)(numIndices > 0 ? numIndices : numVerts);
		return all;
	}
	// Effective LOD list: `lods` or the implicit single level.
	int LodCount() const { return lods.empty() ? 1 : (int)lods.size(); }
	MeshLOD Lod(int l) const
	{
		if (l >= 0 && l < (int)lods.size()) return lods[l];
		MeshLOD all; all.firstSection = 0; all.sectionCount = SectionCount();
		return all;
	}

	// --- skinning (empty on rigid meshes) ----
	// Per-vertex bone bindings, 4 per vertex: indices into `bones`, weights normalized to sum 1.
	// u16 indices: FBX rigs grow $AssimpFbx$ helper nodes, so skeletons pass 255 joints.
	unsigned short* boneIndex  = nullptr;   // 4 * numVerts
	float*          boneWeight = nullptr;   // 4 * numVerts
	std::vector<MeshBone> bones;
	// Skinned = weights + a palette to index: the embedded legacy skeleton OR a .nuskel ref.
	bool HasSkin() const { return boneIndex && boneWeight && (!bones.empty() || !skelGuid.empty()); }

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

	// ---- CPU ray queries --------------------------------------------------------------------
	// A triangle BVH over LOD0, built ONCE per mesh and reused by every CPU ray consumer (VFX
	// collision outside play, tools, gameplay picks). Without it a query degrades into "test
	// every triangle", which turns one large mesh into a frame-killer for anything that shoots
	// rays per particle. Local space, like the triangles themselves.
	struct RayHit
	{
		float t = 0.0f;               // distance along the ray
		float point[3]  = { 0, 0, 0 };
		float normal[3] = { 0, 1, 0 };
		float u = -1.0f, v = -1.0f;   // interpolated UV0 when the mesh has one
		int   tri = -1;
	};
	// Nearest hit in LOCAL space within maxT. Builds the BVH on first use; a mesh whose vertices
	// were rewritten must call InvalidateRayTree().
	bool RaycastLocal(const float ro[3], const float rd[3], float maxT, RayHit& out);
	void InvalidateRayTree();
	struct RayTree;                   // opaque: nodes + triangle order
	RayTree* rayTree = nullptr;

	bc::list<Mesh*>  children;


	Mesh();

	// `scene` (optional) enables skin import: bone weights + the skeleton from the node tree.
	// Produces an INDEXED single-section mesh (v4); wrapper over the multi-mesh builder below.
	void ImportAIMesh(aiMesh* mesh, const aiScene* scene = nullptr);

	// Build ONE indexed mesh from a node's aiMesh list: a section per source mesh, material
	// SLOTS deduped in list order (outSlotMats = slot -> aiScene material index), merged
	// skeleton, tangents/uv2/color streams, meshopt vertex-cache+fetch optimization and an
	// auto-generated LOD chain (simplified index ranges appended after the LOD0 sections).
	static Mesh* ImportAIMeshes(const std::vector<aiMesh*>& meshes, const aiScene* scene,
	                            std::vector<unsigned int>* outSlotMats = nullptr,
	                            const std::vector<MeshBone>* sharedSkeleton = nullptr);
	// The merged skeleton of EVERY skinned mesh in the scene — the .nuskel source. Empty
	// result = no bones anywhere. Bone indices of meshes built with `sharedSkeleton` point
	// into this palette (mesh embeds NO skeleton of its own then).
	static void ImportAISkeleton(const aiScene* scene, std::vector<MeshBone>& outBones);

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
