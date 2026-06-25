#pragma once
#ifndef NUKEE_MESHRENDERER_H
#define NUKEE_MESHRENDERER_H
#include "API/Model/Include.h"

namespace nuke {

class MeshRenderer : public Component
{

public:
    Mesh        *mesh;
    Material    *mat;

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
