#include "API/Model/MeshRenderer.h"
#include "API/Model/resdb.h"
#include "reflect/ReflectBind.h"
#include <render/irender.h>

namespace nuke {

MeshRenderer::MeshRenderer() : Component("MeshRenderer"), mesh(nullptr), mat(nullptr) {}
MeshRenderer::MeshRenderer(const char* typeName) : Component(typeName), mesh(nullptr), mat(nullptr) {}

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
	ResolveMaterials();
}

void MeshRenderer::ResolveMaterials()
{
	// Per-slot owned instances; sized to matGuids, empty guids stay null (MaterialForSlot
	// falls back to the single `mat`).
	for (Material* m : mats) if (m) { Reflect_DropObject(m); delete m; }
	mats.assign(matGuids.size(), nullptr);
	for (size_t i = 0; i < matGuids.size(); ++i)
	{
		if (matGuids[i].empty()) continue;
		Material* asset = ResDB::getSingleton()->GetMaterial(matGuids[i]);
		if (asset) mats[i] = asset->Clone();
	}
}

void MeshRenderer::Destroy() {
	if (mat) { Reflect_DropObject(mat); delete mat; mat = nullptr; }   // owned instance; script handles die with it
	for (Material* m : mats) if (m) { Reflect_DropObject(m); delete m; }
	mats.clear();
}

// Pure data (mesh + material + enabled); drawing happens in the render pass, not here.
void MeshRenderer::Update() {}

void MeshRenderer::FixedUpdate() {}

void MeshRenderer::Pause() {}

void MeshRenderer::Reset() {
	mesh = nullptr;
}

}  // namespace nuke