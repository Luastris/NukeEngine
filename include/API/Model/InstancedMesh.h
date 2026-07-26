#pragma once
#ifndef NUKEE_INSTANCEDMESH_H
#define NUKEE_INSTANCEDMESH_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <vector>
#include <string>

namespace nuke {

// GPU-instanced mesh scatter (7.1): ONE mesh + material drawn N times in a handful of draw
// calls. Instances are LOCAL to the atom (the atom's world transform applies on top), stored
// compactly (base64 blob in the hidden `data` prop — thousands of instances never hit the
// JSON tree as objects) and grouped into SPATIAL CHUNKS for per-camera frustum culling.
// Consumers: foliage scatter, debris, VFX mesh swarms, modular architecture.
// The renderer draws chunks through renderObjectInstanced / renderShadowInstanced /
// renderGBufferInstanced (world / shadow / velocity passes); RT reflections take the
// instances individually up to `rtMaxInstances` (a TLAS entry per instance is not free).
class NUKEENGINE_API InstancedMesh : public Component
{
	NUKE_CLASS(InstancedMesh, Component, "Rendering")
public:
	Mesh*     mesh = nullptr;   // resolved from meshGuid (runtime; re-resolves when the prop changes)
	Material* mat  = nullptr;   // OWNED instance (clone of the matGuid asset), like MeshRenderer
	std::string meshGuidRes, matGuidRes;   // guid each cache was resolved from (hot-apply, no latch)
	float cellSizeRes = -1.f;              // cell size the chunks were built with (live re-chunk)
	// RT presence, MERGED (7.4): one Mesh per spatial chunk with all its instances baked in
	// ATOM-LOCAL space — a single TLAS entry per chunk instead of one per instance, so the
	// WHOLE set ray-traces (shadows, reflections, self-shadow) at flat TLAS cost. Rebuilt
	// alongside the chunks; pooled Meshes are reused in place (version bump re-uploads).
	std::vector<Mesh*> rtChunkMeshes;

	[[nuke::prop(asset="mesh", label="Mesh")]]         std::string meshGuid;
	[[nuke::prop(asset="material", label="Material")]] std::string matGuid;
	[[nuke::prop(label="Cast Shadows")]]               bool  castShadows = true;
	[[nuke::prop(label="Receive Shadows", tip="Off: instances ignore all shadowing (self-shadow included) and stay fully lit.")]] bool receiveShadows = true;
	[[nuke::prop(label="In Reflections", tip="Show instances to RT reflection rays. Cast Shadows alone already puts them in the RT scene as shadow-only casters.")]] bool inReflections = false;
	[[nuke::prop(label="RT Max Instances", min=0, tip="0 = exclude the set from the RT scene. Chunks are merged into single BLASes, so no per-instance cap applies.")]] int rtMaxInstances = 256;
	[[nuke::prop(label="Cell Size", min=1, tip="Spatial chunk size (world units) for frustum culling.")]] float cellSize = 16.0f;
	[[nuke::prop(hidden)]] std::string data;   // packed instances (base64 of floats) — the serialized store

	// ---- reflected instance API (scripts author scatters through these) --------------------
	[[nuke::func]] int  AddInstance(const Vector3& pos, const Vector3& eulerDeg, const Vector3& scale);
	[[nuke::func]] void SetInstancePos(int index, const Vector3& pos);
	[[nuke::func]] void SetInstanceTint(int index, double r, double g, double b, double a);
	[[nuke::func]] void SetInstanceCustom(int index, double x, double y, double z, double w);
	[[nuke::func]] void RemoveInstance(int index);   // order-preserving remove
	[[nuke::func]] void ClearInstances();
	[[nuke::func]] int  InstanceCount();

	// ---- runtime (not serialized) ----------------------------------------------------------
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

	// Called by the render pass (World::Render): resolve assets, decode the blob, rebuild
	// chunks + upload when dirty or the atom moved. Returns false when there is nothing to draw.
	// VIRTUAL: derived scatters (Foliage, 7.4) sync their per-instance params first.
	virtual bool EnsureRenderReady(iRender* r);
	// Whether the ATOM's scale multiplies into the instances. Foliage returns false: a layer
	// on a stretched ground atom (e.g. scale 40x1x40) must not blow every blade up into a
	// 40 m sheet — it follows the atom's position/rotation only, instance scale stays world.
	virtual bool ScaleWithAtom() const { return true; }

protected:
	void EnsureDecoded();
	void MarkDirty() { dirty = true; }
	void ReleaseRTChunks();   // invalidate renderer caches + free the merged RT chunk meshes
};

}  // namespace nuke

#endif // !NUKEE_INSTANCEDMESH_H
