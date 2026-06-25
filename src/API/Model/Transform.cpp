#include "API/Model/Transform.h"
#include "API/Model/Atom.h"
#include "API/Model/Vector.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/ext.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>

#define _USE_MATH_DEFINES // for C
#include <math.h>

#include <iostream>

namespace nuke {
using namespace std;

// engine Quaternion (x,y,z,w) <-> glm::quat (w,x,y,z)
static glm::quat ToGlm(const Quaternion& q) { return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z); }
static Quaternion FromGlm(const glm::quat& g) { return Quaternion(g.x, g.y, g.z, g.w); }

Transform::Transform(Atom* parent)
{
	Init(parent);
}

void Transform::Init(Atom* parent)
{
	go = parent;
}

Vector3 Transform::forward()
{
	return direction();
}

// Basis vectors derived from the rotation quaternion (left-handed; identity looks +Z).
Vector3 Transform::direction() {
	glm::vec3 f = ToGlm(rotation) * glm::vec3(0, 0, 1);
	return Vector3(f.x, f.y, f.z);
}

Vector3 Transform::right()
{
	glm::vec3 r = ToGlm(rotation) * glm::vec3(1, 0, 0);
	return Vector3(r.x, r.y, r.z);
}

Vector3 Transform::up()
{
	glm::vec3 u = ToGlm(rotation) * glm::vec3(0, 1, 0);
	return Vector3(u.x, u.y, u.z);
}

void Transform::SetEulerDeg(const Vector3& deg)
{
	eulerHint = deg;
	glm::vec3 r(glm::radians((float)deg.x), glm::radians((float)deg.y), glm::radians((float)deg.z));
	rotation = FromGlm(glm::quat(r));
}

Vector3 Transform::EulerDeg()
{
	// Show the cached authored euler while it still represents the current rotation
	// (stable, no quat->euler jitter). If the quaternion was changed by other means,
	// recompute the euler and refresh the cache.
	glm::quat hintQ = glm::quat(glm::vec3(glm::radians((float)eulerHint.x),
	                                      glm::radians((float)eulerHint.y),
	                                      glm::radians((float)eulerHint.z)));
	glm::quat cur = ToGlm(rotation);
	if (fabs(glm::dot(hintQ, cur)) > 0.99999f)
		return eulerHint;
	glm::vec3 e = glm::eulerAngles(cur);
	eulerHint = Vector3(glm::degrees(e.x), glm::degrees(e.y), glm::degrees(e.z));
	return eulerHint;
}


void Transform::Destroy()
{

}

void Transform::Update()
{

}

void Transform::FixedUpdate()
{

}

void Transform::Pause()
{

}

void Transform::Reset()
{
	position = Vector3::zero;
	rotation = Quaternion();   // identity
	scale = Vector3::one;
}



Vector3 Transform::globalPosition() {
	return Vector3((this->go != nullptr && this->go->GetParent() != nullptr)
		? (this->position + this->go->GetParent()->GetTransform().globalPosition())
		: (this->position));
}

Quaternion Transform::globalRotation() {
	return (this->go && this->go->GetParent())
		? FromGlm(ToGlm(this->go->GetParent()->GetTransform().globalRotation()) * ToGlm(this->rotation))
		: this->rotation;
}

Vector3 Transform::globalScale() {
	return Vector3((this->go->GetParent()) ? (this->scale * this->go->GetParent()->GetTransform().globalScale()) : (this->scale));
}

}  // namespace nuke