#include "API/Model/Physics.h"
#include "API/Model/Atom.h"
#include "API/Model/CharacterController.h"   // RaycastIgnore: the atom's capsule body
#include "API/Model/Collider.h"
#include "API/Model/World.h"
#include "interface/AppInstance.h"
#include "interface/Services.h"
#include "service/iPhysics.h"
#include <cmath>
#include <vector>

namespace nuke {

// Last hit, PER THREAD: the game thread's raycast (Lua update) and the fixed thread's
// raycast (FixedUpdate logic) must not clobber each other's results.
static thread_local RayHit tl_lastHit;

// bodyId -> Collider, by walking the live world (queries are occasional; no cache to atom stale).
static Collider* FindColliderByBody(bc::list<Atom*>& gos, uint64_t body)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		if (Collider* col = atom->GetComponent<Collider>())
			if (col->bodyId == body) return col;
		if (!atom->children.empty())
			if (Collider* c = FindColliderByBody(atom->children, body)) return c;
	}
	return nullptr;
}

bool Physics::Available() { return GetService<iPhysics>() != nullptr; }

bool Physics::Raycast(const Vector3& from, const Vector3& dir, double maxDist)
{
	tl_lastHit = RayHit{};
	iPhysics* p = GetService<iPhysics>();
	World* w = AppInstance::GetSingleton()->currentWorld;
	if (!p || !w) return false;

	float f[3] = { (float)from.x, (float)from.y, (float)from.z };
	float d[3] = { (float)dir.x,  (float)dir.y,  (float)dir.z };
	uint64_t body = 0;
	float point[3], normal[3];
	if (!p->raycast(f, d, (float)maxDist, body, point, normal)) return false;

	Collider* col = FindColliderByBody(w->GetHierarchy(), body);
	tl_lastHit.atom   = col ? col->atom : nullptr;
	tl_lastHit.point  = Vector3(point[0], point[1], point[2]);
	tl_lastHit.normal = Vector3(normal[0], normal[1], normal[2]);
	Vector3 delta(tl_lastHit.point.x - from.x, tl_lastHit.point.y - from.y, tl_lastHit.point.z - from.z);
	tl_lastHit.distance = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
	return true;
}

// The atom's physics body for exclusion casts: its character's inner capsule, else its collider.
static uint64_t BodyOfAtom(iPhysics* p, Atom* a)
{
	if (!a) return 0;
	if (CharacterController* cc = a->GetComponent<CharacterController>())
		if (cc->charId)
			if (uint64_t b = p->characterBodyId(cc->charId)) return b;
	if (Collider* col = a->GetComponent<Collider>()) return col->bodyId;
	return 0;
}

bool Physics::RaycastIgnore(const Vector3& from, const Vector3& dir, double maxDist, Atom* ignore)
{
	tl_lastHit = RayHit{};
	iPhysics* p = GetService<iPhysics>();
	World* w = AppInstance::GetSingleton()->currentWorld;
	if (!p || !w) return false;

	const uint64_t ignoreBody = BodyOfAtom(p, ignore);
	float f[3] = { (float)from.x, (float)from.y, (float)from.z };
	float d[3] = { (float)dir.x,  (float)dir.y,  (float)dir.z };
	uint64_t body = 0;
	float point[3], normal[3];
	if (!p->raycastIgnore(f, d, (float)maxDist, ignoreBody, body, point, normal)) return false;

	Collider* col = FindColliderByBody(w->GetHierarchy(), body);
	tl_lastHit.atom   = col ? col->atom : nullptr;
	tl_lastHit.point  = Vector3(point[0], point[1], point[2]);
	tl_lastHit.normal = Vector3(normal[0], normal[1], normal[2]);
	Vector3 delta(tl_lastHit.point.x - from.x, tl_lastHit.point.y - from.y, tl_lastHit.point.z - from.z);
	tl_lastHit.distance = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
	return true;
}

bool Physics::SphereCastIgnore(const Vector3& from, double radius, const Vector3& dir,
                               double maxDist, Atom* ignore)
{
	tl_lastHit = RayHit{};
	iPhysics* p = GetService<iPhysics>();
	World* w = AppInstance::GetSingleton()->currentWorld;
	if (!p || !w) return false;

	NukeShapeDesc s; s.shape = 1; s.radius = (float)radius;
	float f[3] = { (float)from.x, (float)from.y, (float)from.z };
	float q[4] = { 0, 0, 0, 1 };
	float d[3] = { (float)dir.x, (float)dir.y, (float)dir.z };
	uint64_t body = 0;
	float point[3], normal[3];
	if (!p->shapeCastIgnore(s, f, q, d, (float)maxDist, BodyOfAtom(p, ignore), body, point, normal))
		return false;

	Collider* col = FindColliderByBody(w->GetHierarchy(), body);
	tl_lastHit.atom   = col ? col->atom : nullptr;
	tl_lastHit.point  = Vector3(point[0], point[1], point[2]);
	tl_lastHit.normal = Vector3(normal[0], normal[1], normal[2]);
	Vector3 delta(tl_lastHit.point.x - from.x, tl_lastHit.point.y - from.y, tl_lastHit.point.z - from.z);
	tl_lastHit.distance = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
	return true;
}

// Shared tail of every cast: resolve the body, fill the thread-local last hit.
static bool FinishCast(bool hit, uint64_t body, const float point[3], const float normal[3], const Vector3& from)
{
	if (!hit) return false;
	World* w = AppInstance::GetSingleton()->currentWorld;
	Collider* col = w ? FindColliderByBody(w->GetHierarchy(), body) : nullptr;
	tl_lastHit.atom   = col ? col->atom : nullptr;
	tl_lastHit.point  = Vector3(point[0], point[1], point[2]);
	tl_lastHit.normal = Vector3(normal[0], normal[1], normal[2]);
	Vector3 delta(tl_lastHit.point.x - from.x, tl_lastHit.point.y - from.y, tl_lastHit.point.z - from.z);
	tl_lastHit.distance = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
	return true;
}

static bool ShapeCastCommon(const NukeShapeDesc& s, const Vector3& from, const Quaternion& rot,
                            const Vector3& dir, double maxDist)
{
	tl_lastHit = RayHit{};
	iPhysics* p = GetService<iPhysics>();
	if (!p) return false;
	float f[3]  = { (float)from.x, (float)from.y, (float)from.z };
	float q[4]  = { (float)rot.x, (float)rot.y, (float)rot.z, (float)rot.w };
	float d[3]  = { (float)dir.x, (float)dir.y, (float)dir.z };
	uint64_t body = 0;
	float point[3], normal[3];
	bool hit = p->shapeCast(s, f, q, d, (float)maxDist, body, point, normal);
	return FinishCast(hit, body, point, normal, from);
}

bool Physics::SphereCast(const Vector3& from, double radius, const Vector3& dir, double maxDist)
{
	NukeShapeDesc s; s.shape = 1; s.radius = (float)radius;
	return ShapeCastCommon(s, from, Quaternion(), dir, maxDist);
}

bool Physics::BoxCast(const Vector3& from, const Vector3& halfExtents,
                      const Quaternion& rot, const Vector3& dir, double maxDist)
{
	NukeShapeDesc s; s.shape = 0;
	s.halfExtents[0] = (float)halfExtents.x;
	s.halfExtents[1] = (float)halfExtents.y;
	s.halfExtents[2] = (float)halfExtents.z;
	return ShapeCastCommon(s, from, rot, dir, maxDist);
}

bool Physics::CapsuleCast(const Vector3& from, double radius, double halfHeight,
                          const Quaternion& rot, const Vector3& dir, double maxDist)
{
	NukeShapeDesc s; s.shape = 2; s.radius = (float)radius; s.halfHeight = (float)halfHeight;
	return ShapeCastCommon(s, from, rot, dir, maxDist);
}

// Overlap results, PER THREAD (same reasoning as the last hit).
static thread_local std::vector<Atom*> tl_overlap;

static int OverlapCommon(const NukeShapeDesc& s, const Vector3& center, const Quaternion& rot)
{
	tl_overlap.clear();
	iPhysics* p = GetService<iPhysics>();
	World* w = AppInstance::GetSingleton()->currentWorld;
	if (!p || !w) return 0;
	float pos[3] = { (float)center.x, (float)center.y, (float)center.z };
	float q[4]   = { (float)rot.x, (float)rot.y, (float)rot.z, (float)rot.w };
	uint64_t bodies[128];
	const int n = p->overlap(s, pos, q, bodies, 128);
	for (int i = 0; i < n; ++i)
		if (Collider* col = FindColliderByBody(w->GetHierarchy(), bodies[i]))
			if (col->atom) tl_overlap.push_back(col->atom);
	return (int)tl_overlap.size();
}

int Physics::OverlapSphere(const Vector3& center, double radius)
{
	NukeShapeDesc s; s.shape = 1; s.radius = (float)radius;
	return OverlapCommon(s, center, Quaternion());
}

int Physics::OverlapBox(const Vector3& center, const Vector3& halfExtents, const Quaternion& rot)
{
	NukeShapeDesc s; s.shape = 0;
	s.halfExtents[0] = (float)halfExtents.x;
	s.halfExtents[1] = (float)halfExtents.y;
	s.halfExtents[2] = (float)halfExtents.z;
	return OverlapCommon(s, center, rot);
}

int Physics::OverlapCapsule(const Vector3& center, double radius, double halfHeight, const Quaternion& rot)
{
	NukeShapeDesc s; s.shape = 2; s.radius = (float)radius; s.halfHeight = (float)halfHeight;
	return OverlapCommon(s, center, rot);
}

Atom* Physics::OverlapAtom(int index)
{
	return (index >= 0 && index < (int)tl_overlap.size()) ? tl_overlap[(size_t)index] : nullptr;
}

Atom*   Physics::HitAtom()     { return tl_lastHit.atom; }
Vector3 Physics::HitPoint()    { return tl_lastHit.point; }
Vector3 Physics::HitNormal()   { return tl_lastHit.normal; }
double  Physics::HitDistance() { return tl_lastHit.distance; }

const RayHit& Physics::LastHit() { return tl_lastHit; }

}  // namespace nuke
