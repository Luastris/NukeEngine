// Physics joint components: thin drivers over the iPhysics constraint seam (see Joints.h).
#include "API/Model/Physics.h"
#include "API/Model/Joints.h"
#include "API/Model/Atom.h"
#include "API/Model/Collider.h"
#include "API/Model/DebugDraw.h"
#include "API/Model/Events.h"
#include "API/Model/Game.h"
#include "API/Model/World.h"
#include "interface/AppInstance.h"
#include "interface/Services.h"
#include "service/iPhysics.h"
#include <nlohmann/json.hpp>
#include <cmath>

namespace nuke {

static const float kDeg2Rad = 0.01745329252f;

void JointBase::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void JointBase::Destroy() { Detach(); }

void JointBase::Reset()
{
	Detach();
	broken = false;
	motorApplied = -999;
}

Transform* JointBase::AnchorT()
{
	return anchor ? &anchor->GetTransform() : transform;
}

void JointBase::TryAttach()
{
	iPhysics* ph = Physics::Scene();
	if (!ph) return;
	Collider* own = atom->GetComponent<Collider>();
	if (!own || !own->bodyId) return;
	uint64_t bodyA = 0;
	if (connectedBody)
	{
		Collider* cc = connectedBody->GetComponent<Collider>();
		if (!cc || !cc->bodyId) return;   // wait for the connected body
		bodyA = cc->bodyId;
	}
	Transform* at = AnchorT();
	Vector3 p = at->globalPosition();
	Vector3 ax = at->globalRotation().Rotate(Vector3(0, 0, 1));
	Vector3 nm = at->globalRotation().Rotate(Vector3(1, 0, 0));
	NukeConstraintDesc d;
	d.bodyA = bodyA;
	d.bodyB = own->bodyId;
	d.pivot[0] = d.pivotB[0] = (float)p.x;
	d.pivot[1] = d.pivotB[1] = (float)p.y;
	d.pivot[2] = d.pivotB[2] = (float)p.z;
	d.axis[0] = (float)ax.x;  d.axis[1] = (float)ax.y;  d.axis[2] = (float)ax.z;
	d.normal[0] = (float)nm.x; d.normal[1] = (float)nm.y; d.normal[2] = (float)nm.z;
	FillDesc(d);
	handle = ph->createConstraint(d);
	if (handle)
	{
		motorApplied = -999;
		ApplyMotor();
	}
}

void JointBase::Detach()
{
	if (!handle) return;
	if (iPhysics* ph = Physics::Scene()) ph->destroyJoint(handle);
	handle = 0;
}

void JointBase::Snap()
{
	Detach();
	broken = true;
	nlohmann::json j;
	j["atom"] = (double)(atom ? atom->id.id : 0);
	j["connected"] = (double)(connectedBody ? connectedBody->id.id : 0);
	j["joint"] = name ? name : "";
	Events::Emit("physics.break", j.dump());
}

void JointBase::FixedUpdate()
{
	if (!atom || !Game::IsPlaying()) return;
	if (broken) return;
	if (!handle) { TryAttach(); return; }
	iPhysics* ph = Physics::Scene();
	if (!ph) return;
	ApplyMotor();
	if (breakForce > 0.0f)
	{
		World* w = Game::GetWorld();
		const float dt = (w && w->settings.fixedDt > 0.0001f) ? w->settings.fixedDt : 1.0f / 60.0f;
		if (ph->constraintImpulse(handle) / dt > breakForce) Snap();
	}
}

bool JointBase::Attached() { return handle != 0; }
bool JointBase::Broken()   { return broken; }
void JointBase::Break()    { if (!broken) Snap(); }

// Anchor marker + axis + a line to the connected body, for the selected atom only.
void JointBase::OnRender(iRender*, RenderPhase phase)
{
	if (phase != RenderPhase::Opaque || !atom) return;
	if (AppInstance::GetSingleton()->selectedInHieararchy != atom) return;
	Transform* at = AnchorT();
	Vector3 p = at->globalPosition();
	Vector3 ax = at->globalRotation().Rotate(Vector3(0, 0, 1));
	Vector3 a0 = p - ax * 0.4;
	Vector3 a1 = p + ax * 0.4;
	Color c = broken ? Color(0.9, 0.3, 0.2, 1.0) : Color(0.4, 0.85, 1.0, 1.0);
	DebugDraw::WireSphere(p, 0.06, c);
	DebugDraw::Line(a0, a1, c);
	if (connectedBody)
		DebugDraw::Line(p, connectedBody->GetTransform().globalPosition(), Color(0.4, 0.85, 1.0, 0.7));
}

// ---- per-type fills -----------------------------------------------------------------------

void HingeJoint::FillDesc(NukeConstraintDesc& d)
{
	d.type = 0;
	d.limit = useLimits;
	d.min = minAngle * kDeg2Rad;
	d.max = maxAngle * kDeg2Rad;
}

void HingeJoint::ApplyMotor()
{
	if (motor == motorApplied && motorTarget == motorTargetApplied && motorMax == motorMaxApplied) return;
	iPhysics* ph = Physics::Scene();
	if (!ph || !handle) return;
	ph->setConstraintMotor(handle, motor, motorTarget * kDeg2Rad, motorMax);
	motorApplied = motor; motorTargetApplied = motorTarget; motorMaxApplied = motorMax;
}

void SliderJoint::FillDesc(NukeConstraintDesc& d)
{
	d.type = 1;
	d.limit = useLimits;
	d.min = minDist;
	d.max = maxDist;
}

void SliderJoint::ApplyMotor()
{
	if (motor == motorApplied && motorTarget == motorTargetApplied && motorMax == motorMaxApplied) return;
	iPhysics* ph = Physics::Scene();
	if (!ph || !handle) return;
	ph->setConstraintMotor(handle, motor, motorTarget, motorMax);
	motorApplied = motor; motorTargetApplied = motorTarget; motorMaxApplied = motorMax;
}

// Distance/spring endpoints: the A end sits on the connected atom (or the anchor when pinned
// to the world), the B end on this atom.
static void DistanceEnds(JointBase* jb, Atom* self, Atom* connected, Transform* anchorT, NukeConstraintDesc& d)
{
	(void)jb;
	Vector3 a = connected ? connected->GetTransform().globalPosition() : anchorT->globalPosition();
	Vector3 b = self->GetTransform().globalPosition();
	d.pivot[0] = (float)a.x;  d.pivot[1] = (float)a.y;  d.pivot[2] = (float)a.z;
	d.pivotB[0] = (float)b.x; d.pivotB[1] = (float)b.y; d.pivotB[2] = (float)b.z;
}

void DistanceJoint::FillDesc(NukeConstraintDesc& d)
{
	d.type = 2;
	DistanceEnds(this, atom, connectedBody, AnchorT(), d);
	d.limit = useLimits;
	d.min = minDistance;
	d.max = maxDistance;
	d.frequency = frequency;
	d.damping = damping;
}

void SpringJoint::FillDesc(NukeConstraintDesc& d)
{
	d.type = 3;
	DistanceEnds(this, atom, connectedBody, AnchorT(), d);
	float rest = restLength;
	if (rest <= 0.0f)
	{
		const float dx = d.pivot[0] - d.pivotB[0], dy = d.pivot[1] - d.pivotB[1], dz = d.pivot[2] - d.pivotB[2];
		rest = std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	d.limit = true;
	d.min = d.max = rest;
	d.frequency = frequency > 0.01f ? frequency : 2.0f;
	d.damping = damping;
}

void ConeJoint::FillDesc(NukeConstraintDesc& d)
{
	d.type = 4;
	d.halfCone = halfAngle * kDeg2Rad;
}

}  // namespace nuke
