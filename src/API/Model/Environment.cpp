#include "API/Model/Environment.h"
#include "API/Model/Atom.h"

namespace nuke {

Environment::Environment() : Component("Environment") {}

void Environment::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Environment::Destroy()     {}
void Environment::Update()       {}
void Environment::FixedUpdate()  {}
void Environment::Pause()        {}
void Environment::Reset()        {}

}  // namespace nuke
