#pragma once
#ifndef NUKEE_JOINTS_H
#define NUKEE_JOINTS_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>

namespace nuke {

struct NukeConstraintDesc;

// Physics joints between this atom's body and a connected body (null = pinned to the world).
// The optional anchor atom gives the pivot and the axes (Z = the joint axis, X = the zero
// reference) — a child atom placed with the normal gizmo; null = this atom's own transform.
// Joints attach in play once both bodies exist, and snap past Break Force, broadcasting
// "physics.break" {atom, connected, joint} on the event bus.
class NUKEENGINE_API JointBase : public Component
{
	NUKE_CLASS_NOCREATE(JointBase, Component)
public:
	[[nuke::prop(label="Connected Body", tip="Atom with a Collider this joint attaches to (null = the world)")]]
	Atom* connectedBody = nullptr;
	[[nuke::prop(label="Anchor", tip="Atom giving pivot + axes (Z = axis, X = reference); null = this atom")]]
	Atom* anchor = nullptr;
	[[nuke::prop(label="Break Force", min=0, tip="Reaction force (N) that snaps the joint; 0 = unbreakable")]]
	float breakForce = 0.0f;

	JointBase(const char* type) : Component(type) {}
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override;
	void Pause() override {}
	void Reset() override;
	void OnRender(iRender* r, RenderPhase phase) override;

	[[nuke::func]] bool Attached();   // constraint live in the backend
	[[nuke::func]] bool Broken();
	[[nuke::func]] void Break();      // manual snap: detach + the break event

	uint64_t handle = 0;   // runtime constraint handle (0 = not attached); not serialized
	bool broken = false;

protected:
	virtual void FillDesc(NukeConstraintDesc& d) = 0;   // type + per-type params
	virtual void ApplyMotor() {}                        // pushed on attach + on prop change
	Transform* AnchorT();
	void TryAttach();
	void Detach();
	void Snap();
	// Last motor state pushed through the seam (re-push only on change: an activate per push
	// would keep the bodies from ever sleeping).
	int   motorApplied = -999;
	float motorTargetApplied = 0.0f, motorMaxApplied = 0.0f;
};

// Rotation about the anchor's Z axis: doors, wheels, levers.
class NUKEENGINE_API HingeJoint : public JointBase
{
	NUKE_CLASS(HingeJoint, JointBase, "Physics")
public:
	[[nuke::prop(label="Use Limits")]] bool useLimits = false;
	[[nuke::prop(label="Min Angle", min=-180, max=180)]] float minAngle = -45.0f;
	[[nuke::prop(label="Max Angle", min=-180, max=180)]] float maxAngle = 45.0f;
	[[nuke::prop(label="Motor", enum="Off,Velocity,Position")]] int motor = 0;
	[[nuke::prop(label="Motor Target", tip="Velocity: deg/s; Position: degrees")]] float motorTarget = 0.0f;
	[[nuke::prop(label="Motor Max Torque", min=0, tip="0 = unlimited")]] float motorMax = 0.0f;

	HingeJoint() : JointBase("HingeJoint") {}
	void FillDesc(NukeConstraintDesc& d) override;
	void ApplyMotor() override;
};

// Translation along the anchor's Z axis: drawers, pistons, elevators.
class NUKEENGINE_API SliderJoint : public JointBase
{
	NUKE_CLASS(SliderJoint, JointBase, "Physics")
public:
	[[nuke::prop(label="Use Limits")]] bool useLimits = false;
	[[nuke::prop(label="Min", tip="Travel limit along the axis, units")]] float minDist = -0.5f;
	[[nuke::prop(label="Max")]] float maxDist = 0.5f;
	[[nuke::prop(label="Motor", enum="Off,Velocity,Position")]] int motor = 0;
	[[nuke::prop(label="Motor Target", tip="Velocity: units/s; Position: units")]] float motorTarget = 0.0f;
	[[nuke::prop(label="Motor Max Force", min=0, tip="0 = unlimited")]] float motorMax = 0.0f;

	SliderJoint() : JointBase("SliderJoint") {}
	void FillDesc(NukeConstraintDesc& d) override;
	void ApplyMotor() override;
};

// Keeps the distance between the two bodies in a range (no limits = rigid rod at the
// attach-time distance): pendants, tow ropes, strut braces.
class NUKEENGINE_API DistanceJoint : public JointBase
{
	NUKE_CLASS(DistanceJoint, JointBase, "Physics")
public:
	[[nuke::prop(label="Use Limits")]] bool useLimits = false;
	[[nuke::prop(label="Min Distance", min=0)]] float minDistance = 0.0f;
	[[nuke::prop(label="Max Distance", min=0)]] float maxDistance = 2.0f;
	[[nuke::prop(label="Frequency", min=0, tip="Soft limit spring, Hz (0 = rigid)")]] float frequency = 0.0f;
	[[nuke::prop(label="Damping", min=0, max=5)]] float damping = 0.0f;

	DistanceJoint() : JointBase("DistanceJoint") {}
	void FillDesc(NukeConstraintDesc& d) override;
};

// A sprung distance: bouncy hangs, suspensions, tension cables.
class NUKEENGINE_API SpringJoint : public JointBase
{
	NUKE_CLASS(SpringJoint, JointBase, "Physics")
public:
	[[nuke::prop(label="Rest Length", min=0, tip="0 = the distance at attach time")]] float restLength = 0.0f;
	[[nuke::prop(label="Frequency", min=0.01, tip="Spring stiffness, Hz")]] float frequency = 2.0f;
	[[nuke::prop(label="Damping", min=0, max=5)]] float damping = 0.5f;

	SpringJoint() : JointBase("SpringJoint") {}
	void FillDesc(NukeConstraintDesc& d) override;
};

// Keeps the body's anchor Z inside a cone about the attach-time direction: chains, tails,
// hanging props that may swing but not flip.
class NUKEENGINE_API ConeJoint : public JointBase
{
	NUKE_CLASS(ConeJoint, JointBase, "Physics")
public:
	[[nuke::prop(label="Half Angle", min=1, max=179)]] float halfAngle = 30.0f;

	ConeJoint() : JointBase("ConeJoint") {}
	void FillDesc(NukeConstraintDesc& d) override;
};

}  // namespace nuke

#endif // !NUKEE_JOINTS_H
