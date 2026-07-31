#include "API/Model/Collider.h"
#include "API/Model/Atom.h"
#include "interface/Services.h"
#include "service/iPhysics.h"

namespace nuke {

Collider::Collider() : Component("Collider") {}

void Collider::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Collider::Destroy()
{
	if (bodyId)
	{
		if (iPhysics* p = GetService<iPhysics>())
			p->destroyBody(bodyId);
		bodyId = 0;
	}
}

void Collider::Update()      {}
void Collider::FixedUpdate() {}
void Collider::Pause()       {}
void Collider::Reset()       { Destroy(); }   // next fixed step recreates from fresh data

}  // namespace nuke
