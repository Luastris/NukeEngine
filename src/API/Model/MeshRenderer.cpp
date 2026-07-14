#include "API/Model/MeshRenderer.h"
#include "API/Model/resdb.h"
#include "reflect/ReflectBind.h"   // Reflect_DropObject: kill script handles to the owned material
#include <render/irender.h>

namespace nuke {

MeshRenderer::MeshRenderer() : Component("MeshRenderer"), mesh(nullptr), mat(nullptr) {}

void MeshRenderer::Init(Atom* parent) {
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	// Resolve the mesh by GUID; clone the material asset into an owned INSTANCE (scene edits atom on
	// the instance, the .numat stays untouched). World load applies saved overrides after this.
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