#include "API/Model/MeshRenderer.h"
#include <render/irender.h>

namespace nuke {

MeshRenderer::MeshRenderer() : Component("MeshRenderer"), mesh(nullptr), mat(nullptr) {}

void MeshRenderer::Init(Atom* parent) {
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	// Rebuild a procedural mesh from its tag (e.g. when loaded from a scene).
	if (!mesh && primitive == "cube") mesh = Mesh::CreateCube();
}

void MeshRenderer::Destroy() {

}

// MeshRenderer is now pure data (mesh + material + enabled). Drawing is done by
// the render pass (World::Render), separate from the logic Update — this keeps
// the editor (always rendering) and Play mode (logic Update) cleanly split.
void MeshRenderer::Update() {}

void MeshRenderer::FixedUpdate() {}

void MeshRenderer::Pause() {}

void MeshRenderer::Reset() {
	mesh = nullptr;
}

}  // namespace nuke