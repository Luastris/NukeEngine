#pragma once
#ifndef NUKEE_MESHRENDERER_H
#define NUKEE_MESHRENDERER_H
#include "API/Model/Include.h"
#include "reflect/Reflect.h"

namespace nuke {

class NUKEENGINE_API MeshRenderer : public Component
{
	NUKE_CLASS(MeshRenderer, Component, "Rendering")
public:
    Mesh        *mesh;                       // resolved at load from meshGuid (runtime, not serialized)
    Material    *mat;                        // OWNED material INSTANCE (clone of the matGuid asset); edits save with the world
    [[nuke::prop(asset="mesh", label="Mesh")]]         std::string meshGuid;   // mesh asset ref (ResDB)
    [[nuke::prop(asset="material", label="Material")]] std::string matGuid;    // material ref, SLOT 0 (single-material meshes)
    // Per-slot material refs for sectioned meshes (v4 imports): index = Mesh material slot.
    // Empty entries (and slots past the end) fall back to `matGuid`/`mat`.
    [[nuke::prop(asset="material", label="Materials")]] std::vector<std::string> matGuids;
    [[nuke::prop(label="In Reflections")]] bool inReflections = true;          // appear in RT reflections (still casts shadows when off)

    std::vector<Material*> mats;             // OWNED per-slot instances mirroring matGuids (runtime)

    // The material drawn for mesh slot `s`: matGuids[s] instance, else the single `mat`.
    Material* MaterialForSlot(int s) const
    { return (s >= 0 && s < (int)mats.size() && mats[s]) ? mats[s] : mat; }
    // (Re)clone per-slot instances from matGuids; drops stale ones. Safe to call repeatedly.
    void ResolveMaterials();

    // Previous-frame global transform (runtime only) — feeds the TAA motion-vector pass.
    float prevPos[3] = {0,0,0}, prevQuat[4] = {0,0,0,1}, prevScale[3] = {1,1,1};
    bool  hasPrev = false;

	MeshRenderer();

	void Init(Atom* parent);

	void Destroy();

	void Update();

	void FixedUpdate();

	void Pause();

	void Reset();

protected:
	// Subclass seam (SkinnedMeshRenderer): keeps the Component name honest.
	explicit MeshRenderer(const char* typeName);
};
}  // namespace nuke

#endif // !NUKEE_MESHRENDERER_h
