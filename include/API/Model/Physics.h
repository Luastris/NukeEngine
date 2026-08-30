#pragma once
#ifndef NUKEE_PHYSICS_H
#define NUKEE_PHYSICS_H
#include "NukeAPI.h"
#include "Vector.h"
#include "reflect/Reflect.h"

namespace nuke {

class Atom;

// A ray-cast hit against the physics scene (C++-side convenience mirror of the last hit).
struct NUKEENGINE_API RayHit
{
	Atom*   atom = nullptr;   // the hit collider's atom
	Vector3 point;            // world hit point
	Vector3 normal;           // world surface normal at the hit
	double  distance = 0.0;   // from the ray origin to the hit point
};

// Game-side facade over the physics service — null-safe (false when no provider or no hit),
// queries run against the LIVE simulation. Composite results use the LAST-HIT pattern:
// a cast stores its hit in THREAD-LOCAL state and the Hit*() getters read it.
class NUKEENGINE_API Physics
{
	NUKE_CLASS_NOCREATE(Physics, Object)
public:
	[[nuke::func]] static bool Available();   // a physics provider is active

	// Nearest hit along the ray (dir need not be normalized). True = hit; read it via
	// HitAtom/HitPoint/HitNormal/HitDistance (valid until the next cast on this thread).
	[[nuke::func]] static bool    Raycast(const Vector3& from, const Vector3& dir, double maxDist);
	// Raycast that IGNORES one atom's physics body (its CharacterController capsule or Collider).
	[[nuke::func]] static bool    RaycastIgnore(const Vector3& from, const Vector3& dir,
	                                            double maxDist, Atom* ignore);
	// Sphere sweep with the same one-atom exclusion.
	[[nuke::func]] static bool    SphereCastIgnore(const Vector3& from, double radius,
	                                               const Vector3& dir, double maxDist, Atom* ignore);

	// SHAPE casts — sweep a volume instead of a thin ray. Same last-hit contract as Raycast;
	// `rot` orients the box/capsule.
	[[nuke::func]] static bool SphereCast(const Vector3& from, double radius,
	                                      const Vector3& dir, double maxDist);
	[[nuke::func]] static bool BoxCast(const Vector3& from, const Vector3& halfExtents,
	                                   const Quaternion& rot, const Vector3& dir, double maxDist);
	[[nuke::func]] static bool CapsuleCast(const Vector3& from, double radius, double halfHeight,
	                                       const Quaternion& rot, const Vector3& dir, double maxDist);

	[[nuke::func]] static Atom*   HitAtom();
	[[nuke::func]] static Vector3 HitPoint();
	[[nuke::func]] static Vector3 HitNormal();
	[[nuke::func]] static double  HitDistance();
	// Mesh UV under the last hit: the hit atom's render mesh (LOD0; skinned meshes read the
	// bind pose) probed at the hit point. Zeros when the atom has no readable mesh. Feed it
	// to Material.TriggerAt for point reactions from gameplay casts.
	[[nuke::func]] static Vector2 HitUV();

	// OVERLAP queries — live atoms inside the volume (triggers included). Returns the count and
	// stores the atoms thread-locally; read via OverlapAtom(i) until the next Overlap* call.
	[[nuke::func]] static int   OverlapSphere(const Vector3& center, double radius);
	[[nuke::func]] static int   OverlapBox(const Vector3& center, const Vector3& halfExtents,
	                                       const Quaternion& rot);
	[[nuke::func]] static int   OverlapCapsule(const Vector3& center, double radius,
	                                           double halfHeight, const Quaternion& rot);
	[[nuke::func]] static Atom* OverlapAtom(int index);   // 0-based; null out of range

	// C++ convenience: the full last hit of the calling thread.
	static const RayHit& LastHit();
	// MESH-space uv of `atom`'s render mesh under a surface point (probed along -normal;
	// LOD0, skinned = bind pose). False when the atom has no readable mesh/uv.
	static bool MeshUVAt(Atom* atom, const Vector3& point, const Vector3& normal, float& u, float& v);

	// EXTERNAL body ownership: modules creating raw iPhysics bodies (terrain chunk colliders)
	// register them so casts resolve the hit atom. Stored by STABLE atom id, resolved against
	// the current world — a stale entry answers null, never a recycled pointer.
	static void RegisterExternalBody(uint64_t body, Atom* atom);
	static void UnregisterExternalBody(uint64_t body);
	// The atom behind an external body (null when unknown) — contact dispatch resolves terrain
	// ground through this, since external bodies carry no Collider component.
	static Atom* ExternalBodyAtom(uint64_t body);

	// Scene-reset epoch: bumped whenever iPhysics::reset() wipes EVERY body (world switch/PIE).
	// Modules owning bodies (terrain chunks) compare epochs to drop dead handles and recook
	// instead of destroying recycled ids.
	static uint32_t ResetEpoch();
	static void     BumpResetEpoch();

	// SCENE ROUTING. Engine physics code resolves the scene through Scene() — normally the
	// iPhysics service — so an editor preview can route everything a subtree does (body
	// create/destroy, stepping, queries) into a PRIVATE sandbox (iPhysics::createScene):
	// wrap every touchpoint in Push/Pop. Thread-local: the game's fixed thread never sees
	// an editor-thread override.
	static class iPhysics* Scene();
	static void PushScene(class iPhysics* s);
	static void PopScene();
};

// RAII Push/Pop for a scene override; null = no-op (plain service resolution stays).
struct PhysicsSceneScope
{
	explicit PhysicsSceneScope(class iPhysics* s) : on(s != nullptr) { if (on) Physics::PushScene(s); }
	~PhysicsSceneScope() { if (on) Physics::PopScene(); }
	PhysicsSceneScope(const PhysicsSceneScope&) = delete;
	PhysicsSceneScope& operator=(const PhysicsSceneScope&) = delete;
private:
	bool on;
};

}  // namespace nuke

#endif // !NUKEE_PHYSICS_H
