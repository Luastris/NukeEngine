#pragma once
#ifndef NUKEE_INSTANCEDMESH_H
#define NUKEE_INSTANCEDMESH_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <vector>
#include <string>

namespace nuke {

// GPU-instanced mesh scatter: one mesh + material drawn N times. Instances are atom-local,
// stored as a base64 blob in the hidden `data` prop and grouped into spatial chunks for
// per-camera frustum culling.
class NUKEENGINE_API InstancedMesh : public Component
{
	NUKE_CLASS(InstancedMesh, Component, "Rendering")
public:
	Mesh*     mesh = nullptr;   // resolved from meshGuid (runtime; re-resolves when the prop changes)
	Material* mat  = nullptr;   // OWNED instance (clone of the matGuid asset), like MeshRenderer
	std::string meshGuidRes, matGuidRes;   // guid each cache was resolved from (hot-apply, no latch)
	float cellSizeRes = -1.f;              // cell size the chunks were built with (live re-chunk)
	std::vector<Mesh*> rtChunkMeshes;   // one merged atom-local Mesh per chunk = one TLAS entry per chunk

	[[nuke::prop(asset="mesh", label="Mesh")]]         std::string meshGuid;
	[[nuke::prop(asset="material", label="Material")]] std::string matGuid;
	[[nuke::prop(label="Cast Shadows")]]               bool  castShadows = true;
	[[nuke::prop(label="Receive Shadows", tip="Off: instances ignore all shadowing (self-shadow included) and stay fully lit.")]] bool receiveShadows = true;
	[[nuke::prop(label="In Reflections", tip="Show instances to RT reflection rays. Cast Shadows alone already puts them in the RT scene as shadow-only casters.")]] bool inReflections = false;
	[[nuke::prop(label="RT Max Instances", min=0, tip="0 = exclude the set from the RT scene. Chunks are merged into single BLASes, so no per-instance cap applies.")]] int rtMaxInstances = 256;
	[[nuke::prop(label="Cell Size", min=1, tip="Spatial chunk size (world units) for frustum culling.")]] float cellSize = 16.0f;
	[[nuke::prop(hidden)]] std::string data;   // packed instances (base64 of floats) — the serialized store

	// ---- reflected instance API ----
	[[nuke::func]] int  AddInstance(const Vector3& pos, const Vector3& eulerDeg, const Vector3& scale);
	[[nuke::func]] void SetInstancePos(int index, const Vector3& pos);
	[[nuke::func]] void SetInstanceTint(int index, double r, double g, double b, double a);
	[[nuke::func]] void SetInstanceCustom(int index, double x, double y, double z, double w);
	[[nuke::func]] void RemoveInstance(int index);   // order-preserving remove
	[[nuke::func]] void ClearInstances();
	[[nuke::func]] int  InstanceCount();

	// ---- runtime (not serialized) ----
	struct Inst { float pos[3]; float quat[4]; float scale[3]; float color[4]; float custom[4]; };
	std::vector<Inst> instances;
	bool     dirty = true;        // instances/props changed -> re-chunk + re-upload
	bool     decoded = false;     // `data` blob decoded into `instances`
	uint64_t gpuBuf = 0;          // renderer instance buffer (createInstanceBuffer)
	iRender* gpuOwner = nullptr;  // renderer the buffer was created on (project switch safety)
	struct Chunk { int first, count; float mn[3], mx[3]; };   // upload-order range + world AABB
	std::vector<Chunk> chunks;
	float lastWorld[16]; bool hasLastWorld = false;   // atom world snapshot (re-upload on move)

	InstancedMesh();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
	void OnBeforeSave() override;   // pack `instances` back into the `data` blob

	// Resolve assets, decode the blob, rebuild chunks + upload when dirty or the atom moved.
	// Returns false when there is nothing to draw.
	virtual bool EnsureRenderReady(iRender* r);
	virtual bool ScaleWithAtom() const { return true; }   // whether the atom's scale multiplies into instances

protected:
	void EnsureDecoded();
	void MarkDirty() { dirty = true; }
	void ReleaseRTChunks();   // invalidate renderer caches + free the merged RT chunk meshes
};

}  // namespace nuke

#endif // !NUKEE_INSTANCEDMESH_H
