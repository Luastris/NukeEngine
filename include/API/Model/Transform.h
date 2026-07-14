#pragma once
#ifndef NUKEE_TRANSFORM_H
#define NUKEE_TRANSFORM_H
#include "NukeAPI.h"

#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"

namespace nuke {

class NUKEENGINE_API Transform : public Component
{
	NUKE_CLASS_NOCREATE(Transform, Component)
public:
	[[nuke::prop]] Vector3 position;
    [[nuke::prop]] Quaternion rotation;   // quaternion (no gimbal lock)
    [[nuke::prop]] Vector3 eulerHint;     // last euler (deg), stable inspector display
    [[nuke::prop]] Vector3 scale = {1,1,1};
    Atom *atom = nullptr;


	Transform(Atom* parent);
	void Init(Atom* parent);

	[[nuke::func]] Vector3 forward();
	[[nuke::func]] Vector3 right();
	[[nuke::func]] Vector3 up();
	[[nuke::func]] Vector3 direction();
	[[nuke::func]] Vector3 globalPosition();
	[[nuke::func]] Quaternion globalRotation();
	[[nuke::func]] Vector3 globalScale();
	// Set local values so the WORLD pose equals these (inverse of the global* getters, relative to the
	// current parent). Keeps an object in place when reparented / drives gizmo write-back.
	[[nuke::func]] void    SetGlobal(const Vector3& pos, const Quaternion& rot, const Vector3& scale);

	// Euler helpers (degrees) for the inspector / authoring. Internally quaternion.
	[[nuke::func]] void    SetEulerDeg(const Vector3& deg);
	[[nuke::func]] Vector3 EulerDeg();
	// Legacy SCRIPT-facing aliases, reflected on purpose: older scripts call
	// transform:setEuler(x, y, z) / transform:euler(). Real methods (not binder shims) so
	// the Lua surface stays 100% reflection-driven.
	[[nuke::func]] void    setEuler(double x, double y, double z);
	[[nuke::func]] Vector3 euler();

	void Destroy();
	void Update();
	void FixedUpdate();
	void Pause();
	void Reset();
};

}  // namespace nuke

#endif // !NUKEE_TRANSFORM_H
