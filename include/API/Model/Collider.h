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
	// colliders are STATIC only; check Convex to build a hull that can be dynamic.
	[[nuke::prop(label="Convex (Mesh)")]] bool convex = false;
	// Trigger: reports OnTriggerEnter/Exit, applies no collision response.
	[[nuke::prop(label="Is Trigger")]]   bool isTrigger = false;
	[[nuke::prop(label="Friction",    min=0, max=1)]] float friction = 0.5f;
	[[nuke::prop(label="Restitution", min=0, max=1)]] float restitution = 0.0f;

	// Runtime physics handle (0 = no body yet). Owned by the world's physics driver —
	// created lazily on the first fixed step, destroyed with the component. NOT serialized.
	uint64_t bodyId = 0;

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
