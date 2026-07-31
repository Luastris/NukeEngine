#include "API/Model/MeshRenderer.h"
#include "API/Model/resdb.h"
#include "reflect/ReflectBind.h"
#include <render/irender.h>

namespace nuke {

MeshRenderer::MeshRenderer() : Component("MeshRenderer"), mesh(nullptr), mat(nullptr) {}

void MeshRenderer::Init(Atom* parent) {
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	// Clone the material asset into an owned instance so edits never touch the .numat.
	if (!mesh && !meshGuid.empty()) mesh = ResDB::getSingleton()->GetMesh(meshGuid);
	if (!mat && !matGuid.empty())
	{
		Material* asset = ResDB::getSingleton()->GetMaterial(matGuid);
		if (asset) mat = asset->Clone();
	}
}

void MeshRenderer::Destroy() {
	if (mat) { Reflect_DropObject(mat); delete mat; mat = nullptr; }   // owned instance; script handles die with it
}

// Pure data (mesh + material + enabled); drawing happens in the render pass, not here.
void MeshRenderer::Update() {}

void MeshRenderer::FixedUpdate() {}

void MeshRenderer::Pause() {}

void MeshRenderer::Reset() {
	mesh = nullptr;
}

}  // namespace nuke