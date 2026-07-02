#include "API/Model/Rigidbody.h"
#include "API/Model/Collider.h"
#include "API/Model/Atom.h"
#include "interface/Services.h"
#include "service/iPhysics.h"

namespace nuke {

Rigidbody::Rigidbody() : Component("Rigidbody") {}

void Rigidbody::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

// The body handle lives on the sibling Collider (it owns the shape).
static uint64_t SiblingBody(Atom* atom)
{
	if (!atom) return 0;
	Collider* col = atom->GetComponent<Collider>();
	return col ? col->bodyId : 0;
}

void Rigidbody::AddForce(const Vector3& force)
{
	iPhysics* p = GetService<iPhysics>();
	uint64_t b = SiblingBody(atom);
	if (!p || !b) return;
	float f[3] = { (float)force.x, (float)force.y, (float)force.z };
	p->addForce(b, f);
}

void Rigidbody::AddImpulse(const Vector3& impulse)
{
	iPhysics* p = GetService<iPhysics>();
	uint64_t b = SiblingBody(atom);
	if (!p || !b) return;
	float i[3] = { (float)impulse.x, (float)impulse.y, (float)impulse.z };
	p->addImpulse(b, i);
}

void Rigidbody::SetVelocity(const Vector3& v)
{
	iPhysics* p = GetService<iPhysics>();
	uint64_t b = SiblingBody(atom);
	if (!p || !b) return;
	float vel[3] = { (float)v.x, (float)v.y, (float)v.z };
	p->setLinearVelocity(b, vel);
}

Vector3 Rigidbody::Velocity()
{
	iPhysics* p = GetService<iPhysics>();
	uint64_t b = SiblingBody(atom);
	if (!p || !b) return Vector3(0, 0, 0);
	float vel[3];
	p->getLinearVelocity(b, vel);
	return Vector3(vel[0], vel[1], vel[2]);
}

void Rigidbody::SetAngularVelocity(const Vector3& v)
{
	iPhysics* p = GetService<iPhysics>();
	uint64_t b = SiblingBody(atom);
	if (!p || !b) return;
	float vel[3] = { (float)v.x, (float)v.y, (float)v.z };
	p->setAngularVelocity(b, vel);
}

Vector3 Rigidbody::AngularVelocity()
{
	iPhysics* p = GetService<iPhysics>();
	uint64_t b = SiblingBody(atom);
	if (!p || !b) return Vector3(0, 0, 0);
	float vel[3];
	p->getAngularVelocity(b, vel);
	return Vector3(vel[0], vel[1], vel[2]);
}

void Rigidbody::Destroy()     {}
void Rigidbody::Update()      {}
void Rigidbody::FixedUpdate() {}
void Rigidbody::Pause()       {}
void Rigidbody::Reset()       {}

}  // namespace nuke
