#include "API/Model/ReflectionProbe.h"
#include "API/Model/Atom.h"

namespace nuke {

ReflectionProbe::ReflectionProbe() : Component("ReflectionProbe") {}

void ReflectionProbe::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	atom = parent;
	parent->components.push_back(this);
}

void ReflectionProbe::Destroy()     {}
void ReflectionProbe::Update()       {}
void ReflectionProbe::FixedUpdate()  {}
void ReflectionProbe::Pause()        {}
void ReflectionProbe::Reset()        {}

}  // namespace nuke
