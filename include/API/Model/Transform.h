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
    Atom *go = nullptr;


	Transform(Atom* parent);
	void Init(Atom* parent);

	Vector3 forward();
	Vector3 right();
	Vector3 up();
	Vector3 direction();
    Vector3 globalPosition();
	Quaternion globalRotation();
	Vector3 globalScale();
	// Set local values so the WORLD pose equals these (inverse of the global* getters, relative to the
	// current parent). Keeps an object in place when reparented / drives gizmo write-back.
	void    SetGlobal(const Vector3& pos, const Quaternion& rot, const Vector3& scale);

	// Euler helpers (degrees) for the inspector / authoring. Internally quaternion.
	void    SetEulerDeg(const Vector3& deg);
	Vector3 EulerDeg();

	void Destroy();
	void Update();
	void FixedUpdate();
	void Pause();
	void Reset();
};

}  // namespace nuke

#endif // !NUKEE_TRANSFORM_H
