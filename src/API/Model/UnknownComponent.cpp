#include "API/Model/UnknownComponent.h"
#include "API/Model/Atom.h"

namespace nuke {

UnknownComponent::UnknownComponent() : Component("Unknown") {}

void UnknownComponent::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

}  // namespace nuke
