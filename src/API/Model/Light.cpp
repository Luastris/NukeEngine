#include "API/Model/Light.h"
#include "API/Model/Atom.h"

namespace nuke {

Light::Light() : Component("Light") {}

void Light::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Light::Destroy()     {}
void Light::Update()      {}
void Light::FixedUpdate() {}
void Light::Pause()       {}
void Light::Reset()       {}

}  // namespace nuke
