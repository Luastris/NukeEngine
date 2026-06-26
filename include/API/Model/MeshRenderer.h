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
    Material    *mat;                        // resolved at load from matGuid (runtime, not serialized)
    [[nuke::prop]] std::string meshGuid;     // mesh asset reference, e.g. "builtin:cube" (resolved via ResDB)
    [[nuke::prop]] std::string matGuid;      // material asset reference (resolved via ResDB)

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
