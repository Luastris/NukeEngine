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



// Standard hierarchical transform: a child's world transform = parent world * local.
// World position = parentPos + parentRot * (parentScale ⊙ localPos) — so children orbit/scale with the
// parent (the old version just ADDED positions, ignoring parent rotation/scale).
Vector3 Transform::globalPosition() {
	Atom* p = this->go ? this->go->GetParent() : nullptr;
	if (!p) return this->position;
	Transform& pt = p->GetTransform();
	Vector3   ps = pt.globalScale();
	glm::vec3 scaled((float)(this->position.x * ps.x), (float)(this->position.y * ps.y), (float)(this->position.z * ps.z));
	glm::vec3 rot = ToGlm(pt.globalRotation()) * scaled;
	Vector3   pp = pt.globalPosition();
	return Vector3(pp.x + rot.x, pp.y + rot.y, pp.z + rot.z);
}

Quaternion Transform::globalRotation() {
	return (this->go && this->go->GetParent())
		? FromGlm(ToGlm(this->go->GetParent()->GetTransform().globalRotation()) * ToGlm(this->rotation))
		: this->rotation;
}

Vector3 Transform::globalScale() {
	return Vector3((this->go && this->go->GetParent()) ? (this->scale * this->go->GetParent()->GetTransform().globalScale()) : (this->scale));
}

// Set this transform so its WORLD pose equals (wp, wr, ws), computing the local values relative to the
// current parent (exact inverse of globalPosition/Rotation/Scale). Used by the gizmo + reparent-keep-world.
void Transform::SetGlobal(const Vector3& wp, const Quaternion& wr, const Vector3& ws) {
	Atom* p = this->go ? this->go->GetParent() : nullptr;
	if (!p) { this->position = wp; this->rotation = wr; this->scale = ws; return; }
	Transform& pt = p->GetTransform();
	Vector3    pp = pt.globalPosition(); Quaternion pr = pt.globalRotation(); Vector3 ps = pt.globalScale();
	glm::quat  ipr = glm::inverse(ToGlm(pr));
	glm::vec3  rel = ipr * glm::vec3((float)(wp.x - pp.x), (float)(wp.y - pp.y), (float)(wp.z - pp.z));
	this->position = Vector3(ps.x != 0 ? rel.x / ps.x : rel.x, ps.y != 0 ? rel.y / ps.y : rel.y, ps.z != 0 ? rel.z / ps.z : rel.z);
	this->rotation = FromGlm(ipr * ToGlm(wr));
	this->scale    = Vector3(ps.x != 0 ? ws.x / ps.x : ws.x, ps.y != 0 ? ws.y / ps.y : ws.y, ps.z != 0 ? ws.z / ps.z : ws.z);
}

}  // namespace nuke