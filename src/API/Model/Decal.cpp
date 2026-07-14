#include "API/Model/Decal.h"

namespace nuke {

Decal::Decal() : Component("Decal") {}

void Decal::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Decal::Update()      {}
void Decal::FixedUpdate() {}
void Decal::Reset()       {}
void Decal::Pause()       {}
void Decal::Destroy()     {}

}  // namespace nuke
