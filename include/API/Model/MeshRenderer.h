#pragma once
#ifndef NUKEE_MESHRENDERER_H
#define NUKEE_MESHRENDERER_H
#include "API/Model/Include.h"
#include "reflect/Reflect.h"

namespace nuke {

class NUKEENGINE_API MeshRenderer : public Component
{
	NUKE_CLASS(MeshRenderer, Component)
public:
    Mesh        *mesh;                       // resolved at load from meshGuid (runtime, not serialized)
    Material    *mat;                        // OWNED material INSTANCE (clone of the matGuid asset);
                                             // scene edits live here + save with the world, not in the .numat
    [[nuke::prop(asset="mesh", label="Mesh")]]         std::string meshGuid;   // mesh asset ref (ResDB)
    [[nuke::prop(asset="material", label="Material")]] std::string matGuid;    // material asset ref (ResDB)
    [[nuke::prop(label="In Reflections")]] bool inReflections = true;          // appear in RT reflections (still casts shadows when off)

    // Previous-frame global transform (runtime only, NOT serialized) — feeds the TAA motion-vector (velocity) pass.
    float prevPos[3] = {0,0,0}, prevQuat[4] = {0,0,0,1}, prevScale[3] = {1,1,1};
    bool  hasPrev = false;

	MeshRenderer();

	void Init(Atom* parent);

	void Destroy();

	void Update();

	void FixedUpdate();

	void Pause();

	void Reset();

};
}  // namespace nuke

#endif // !NUKEE_MESHRENDERER_h
