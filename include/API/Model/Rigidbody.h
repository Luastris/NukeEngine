#pragma once
#ifndef NUKEE_RIGIDBODY_H
#define NUKEE_RIGIDBODY_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"

namespace nuke {

// Dynamic-body settings (roadmap 1.1). Pairs with a Collider on the SAME atom: the
// Collider supplies the shape, this component makes it simulated (or kinematic). The
// world's fixed-step driver writes the simulated pose back into the Transform.
class NUKEENGINE_API Rigidbody : public Component
{
	NUKE_CLASS(Rigidbody, Component)
public:
	[[nuke::prop(label="Mass")]]        float mass = 1.0f;
	[[nuke::prop(label="Use Gravity")]] bool  useGravity = true;
	// Kinematic: not simulated — gameplay moves the Transform, the body follows and
	// pushes dynamic bodies out of the way (platforms, doors).
	[[nuke::prop(label="Kinematic")]]   bool  isKinematic = false;
	[[nuke::prop(label="Linear Damping",  min=0, max=1)]] float linearDamping = 0.05f;
	[[nuke::prop(label="Angular Damping", min=0, max=1)]] float angularDamping = 0.05f;

	// Gameplay API (scriptable via [[nuke::func]]). All act on the sibling Collider's
	// body; harmless no-ops until the body exists (first fixed step of play mode).
	[[nuke::func]] void    AddForce(const Vector3& force);       // continuous, this step
	[[nuke::func]] void    AddImpulse(const Vector3& impulse);   // instantaneous
	[[nuke::func]] void    SetVelocity(const Vector3& v);
	[[nuke::func]] Vector3 Velocity();
	[[nuke::func]] void    SetAngularVelocity(const Vector3& v); // rad/s
	[[nuke::func]] Vector3 AngularVelocity();

	Rigidbody();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};

}  // namespace nuke

#endif // !NUKEE_RIGIDBODY_H
