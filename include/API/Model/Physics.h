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

// Game-side facade over the physics service (Unity Physics.*-style) — null-safe: false
// when no provider is enabled or nothing was hit. Queries run against the LIVE simulation.
//
// The API is [[nuke::func]]-reflected, so every scripting backend binds it AUTOMATICALLY
// (Lua: nuke.Physics.Raycast(...) etc. via the generic static binder — no per-function
// wrappers). Composite results use the LAST-HIT pattern: Raycast() stores its hit in
// thread-local state and the Hit*() getters read it — every piece is an FT type, and
// thread-local keeps the game thread and the fixed thread from clobbering each other.
class NUKEENGINE_API Physics
{
	NUKE_CLASS_NOCREATE(Physics, Object)
public:
	[[nuke::func]] static bool Available();   // a physics provider is active

	// Nearest hit along the ray (dir need not be normalized). True = hit; read it via
	// HitAtom/HitPoint/HitNormal/HitDistance (valid until the next cast on this thread).
	[[nuke::func]] static bool    Raycast(const Vector3& from, const Vector3& dir, double maxDist);
	// Raycast that IGNORES one atom's physics body (its CharacterController capsule or its
	// Collider) — camera booms / aim rays from inside the followed character need this: a
	// plain ray starting inside the capsule reports an inside-hit at distance 0.
	[[nuke::func]] static bool    RaycastIgnore(const Vector3& from, const Vector3& dir,
	                                            double maxDist, Atom* ignore);
	// Sphere sweep with the same one-atom exclusion — camera booms probe with a VOLUME so
	// they don't flicker on edges a thin ray alternately hits and misses (UE spring arm).
	[[nuke::func]] static bool    SphereCastIgnore(const Vector3& from, double radius,
	                                               const Vector3& dir, double maxDist, Atom* ignore);

	// SHAPE casts — sweep a volume instead of an infinitely thin ray (catches what a ray
	// slips past). Same last-hit contract as Raycast. `rot` orients the box/capsule.
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

	// OVERLAP queries — who is inside this volume right now (triggers included). Each
	// returns the count and stores the atoms thread-locally; read them via OverlapAtom(i)
	// (valid until the next Overlap* on this thread). Bodies whose collider died resolve
	// to null and are skipped, so the count only covers live atoms.
	[[nuke::func]] static int   OverlapSphere(const Vector3& center, double radius);
	[[nuke::func]] static int   OverlapBox(const Vector3& center, const Vector3& halfExtents,
	                                       const Quaternion& rot);
	[[nuke::func]] static int   OverlapCapsule(const Vector3& center, double radius,
	                                           double halfHeight, const Quaternion& rot);
	[[nuke::func]] static Atom* OverlapAtom(int index);   // 0-based; null out of range

	// C++ convenience: the full last hit of the calling thread.
	static const RayHit& LastHit();
};

}  // namespace nuke

#endif // !NUKEE_PHYSICS_H
