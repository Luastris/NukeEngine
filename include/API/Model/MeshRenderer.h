#pragma once
#ifndef NUKEE_MESHRENDERER_H
#define NUKEE_MESHRENDERER_H
#include "API/Model/Include.h"
#include "reflect/Reflect.h"

namespace nuke {

class NUKEENGINE_API MeshRenderer : public Component
{
	NUKE_CLASS(MeshRenderer, Component)
	// mesh/mat are asset references — serialized by GUID once the asset system exists.
public:
    Mesh        *mesh;
    Material    *mat;
    [[nuke::prop]] std::string primitive;   // procedural mesh tag ("cube"); empty = asset-ref (future GUID)

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
