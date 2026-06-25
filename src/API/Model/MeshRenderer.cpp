#include "API/Model/MeshRenderer.h"
#include <render/irender.h>

namespace nuke {

MeshRenderer::MeshRenderer() : Component("MeshRenderer") {}

void MeshRenderer::Init(Atom* parent) {
	transform = &parent->GetTransform();
	parent->components.push_back(this);
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