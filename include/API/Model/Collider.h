#pragma once
#ifndef NUKEE_COLLIDER_H
#define NUKEE_COLLIDER_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>

namespace nuke {

// Collision shape component (roadmap 1.1). DATA ONLY — the world's fixed-step driver
// creates/updates the actual body through the physics service (service/iPhysics.h):
//   * Collider alone            = STATIC geometry (walls, floors)
//   * Collider + Rigidbody      = DYNAMIC body (simulated) or KINEMATIC (Rigidbody.isKinematic)
// The atom's world scale is baked into the shape at body creation — rescaling a live
// body takes a recreate (stop/start play). Mesh shape arrives later in the track.
class NUKEENGINE_API Collider : public Component
{
	NUKE_CLASS(Collider, Component)
public:
	enum Shape { S_Box = 0, S_Sphere = 1, S_Capsule = 2, S_Mesh = 3 };

	[[nuke::prop(label="Shape", enum="Box,Sphere,Capsule,Mesh")]] int shape = S_Box;
	[[nuke::prop(label="Half Extents")]] Vector3 halfExtents = Vector3(0.5, 0.5, 0.5);   // Box
	[[nuke::prop(label="Radius")]]       float radius = 0.5f;                            // Sphere / Capsule
	[[nuke::prop(label="Half Height")]]  float halfHeight = 0.5f;                        // Capsule: cylinder half-length
	// Mesh shape takes its triangles from the SIBLING MeshRenderer's mesh. Non-convex mesh
	// colliders can be STATIC or KINEMATIC; check Convex to build a hull that can be dynamic.
	[[nuke::prop(label="Convex (Mesh)")]] bool convex = false;
	// Trigger: reports OnTriggerEnter/Exit, applies no collision response.
	[[nuke::prop(label="Is Trigger")]]   bool isTrigger = false;
	[[nuke::prop(label="Friction",    min=0, max=1)]] float friction = 0.5f;
	[[nuke::prop(label="Restitution", min=0, max=1)]] float restitution = 0.0f;

	// Runtime physics handle (0 = no body yet). Owned by the world's physics driver —
	// created lazily on the first fixed step, destroyed with the component. NOT serialized.
	uint64_t bodyId = 0;

	// Last pose synced between the Transform and the body (runtime, driver-owned, NOT
	// serialized). Lets the driver detect EXTERNAL transform changes (scripts, a moving
	// parent, editor teleports) and push them into the simulation — without mistaking the
	// driver's own dynamic-pose write-backs for external moves.
	Vector3    lastSyncPos;
	Quaternion lastSyncRot;
	bool       hasSync = false;

	// Motion type the body was CREATED with (0 static / 1 dynamic / 2 kinematic; -1 none).
	// When the desired type changes live (Rigidbody added/removed, Kinematic toggled in
	// the inspector mid-play) the driver recreates the body — a stale static body silently
	// ignores kinematic moves otherwise.
	int bodyMotion = -1;

	// Fixed steps since the transform last changed (runtime). Kinematic drive spreads a
	// transform jump over the REAL elapsed steps — a low-fps script write must not get
	// compressed into one step's velocity (it would catapult riders).
	int quietSteps = 0;

	// Kinematic tracking servo state (runtime, driver-owned). Scripts write the
	// transform at RENDER cadence, the simulation consumes it at the FIXED cadence,
	// and the two BEAT: per-step deltas jitter between zero and twice the true speed.
	// Riders are punted by velocity JUMPS, so the body is driven by a velocity that
	// is smooth by construction: the exponentially-smoothed write velocity
	// (feedforward) plus a soft proportional pull towards the newest write. A script
	// that moves the body in fixedUpdate degenerates this into exact tracking.
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
