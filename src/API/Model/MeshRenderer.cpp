#include "API/Model/MeshRenderer.h"
#include "API/Model/resdb.h"
#include <render/irender.h>

namespace nuke {

MeshRenderer::MeshRenderer() : Component("MeshRenderer"), mesh(nullptr), mat(nullptr) {}

void MeshRenderer::Init(Atom* parent) {
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	// Resolve the mesh + material assets by GUID (e.g. when loaded from a world/prefab).
	if (!mesh && !meshGuid.empty()) mesh = ResDB::getSingleton()->GetMesh(meshGuid);
	if (!mat  && !matGuid.empty())  mat  = ResDB::getSingleton()->GetMaterial(matGuid);
	if (mat) mat->Resolve();   // bind the material's textures from ResDB
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