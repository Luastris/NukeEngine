#pragma once
#ifndef NUKEE_COLLIDER_H
#define NUKEE_COLLIDER_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>

namespace nuke {

// Collision shape component. DATA ONLY — the world's fixed-step driver creates/updates the
// actual body via the physics service: Collider alone = STATIC geometry, Collider + Rigidbody
// = DYNAMIC or KINEMATIC. The atom's world scale is baked into the shape at body creation, so
// rescaling a live body takes a recreate.
class NUKEENGINE_API Collider : public Component
{
	NUKE_CLASS(Collider, Component, "Physics")
public:
	enum Shape : int { S_Box = 0, S_Sphere = 1, S_Capsule = 2, S_Mesh = 3 };

	[[nuke::prop(label="Shape", enum="Box,Sphere,Capsule,Mesh")]] Shape shape = S_Box;
	[[nuke::prop(label="Half Extents")]] Vector3 halfExtents = Vector3(0.5, 0.5, 0.5);   // Box
	[[nuke::prop(label="Radius")]]       float radius = 0.5f;                            // Sphere / Capsule
	[[nuke::prop(label="Half Height")]]  float halfHeight = 0.5f;                        // Capsule: cylinder half-length
	// Mesh shape takes its triangles from the SIBLING MeshRenderer's mesh. Non-convex mesh
	// colliders can only be STATIC or KINEMATIC; a convex hull can be dynamic.
	[[nuke::prop(label="Convex (Mesh)")]] bool convex = false;
	// Trigger: reports OnTriggerEnter/Exit, applies no collision response.
	[[nuke::prop(label="Is Trigger")]]   bool isTrigger = false;
	[[nuke::prop(label="Friction",    min=0, max=1)]] float friction = 0.5f;
	[[nuke::prop(label="Restitution", min=0, max=1)]] float restitution = 0.0f;

	uint64_t bodyId = 0;   // runtime physics handle (0 = none); driver-owned, not serialized

	// Last pose synced between the Transform and the body (driver-owned, not serialized).
	// Lets the driver spot EXTERNAL transform changes without mistaking its own write-backs.
	Vector3    lastSyncPos;
	Quaternion lastSyncRot;
	bool       hasSync = false;

	// Motion type the body was CREATED with (0 static / 1 dynamic / 2 kinematic; -1 none).
	// The driver recreates the body when the desired type changes live — a stale static
	// body would silently ignore kinematic moves.
	int bodyMotion = -1;
	// The world scale baked into the live body. Physics engines do not scale bodies, so the
	// shape carries the atom's scale — and a scale edit has to rebuild it, or the collider keeps
	// the size it had when it was created while the object visibly grows or shrinks.
	float bodyScale[3] = { 0, 0, 0 };

	// Fixed steps since the transform last changed (runtime). Kinematic drive spreads a
	// transform jump over the REAL elapsed steps, so a low-fps script write is not
	// compressed into one step's velocity (that would catapult riders).
	int quietSteps = 0;

	// Kinematic tracking servo state (runtime, driver-owned). Render-cadence writes vs
	// fixed-cadence consumption beat against each other, so the body is driven by a
	// velocity smooth by construction: smoothed write velocity plus a proportional pull.
	Vector3 kinVelEma;      // smoothed velocity of incoming writes (m/s, world)
	Vector3 kinAngVelEma;   // smoothed angular velocity of writes (rad/s, axis*rate)

	Collider();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};

}  // namespace nuke

#endif // !NUKEE_COLLIDER_H
