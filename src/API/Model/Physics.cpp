#include "API/Model/Physics.h"
#include "API/Model/Atom.h"
#include "API/Model/CharacterController.h"
#include "API/Model/Collider.h"
#include "API/Model/MeshRenderer.h"   // HitUV: render-mesh probe under the last hit
#include "API/Model/World.h"
#include "interface/AppInstance.h"
#include "interface/Services.h"
#include "service/iPhysics.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace nuke {

// Last hit, PER THREAD: game-thread and fixed-thread casts must not clobber each other.
static thread_local RayHit tl_lastHit;
// Lazy mesh-UV of the last hit (computed on the first HitUV() after a cast).
static thread_local bool    tl_uvDone = false;
static thread_local Vector2 tl_uv;

// bodyId -> Collider by walking the live world (no cache — it would go stale).
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

// Module-created bodies (terrain chunk colliders): body -> owning atom's STABLE id. Resolution
// walks the current world by id, so a stale entry (world switched, atom gone) answers null.
static std::map<uint64_t, unsigned long> g_extBody;

void Physics::RegisterExternalBody(uint64_t body, Atom* atom)
{
	if (body && atom) g_extBody[body] = atom->id.id;
}
void Physics::UnregisterExternalBody(uint64_t body) { g_extBody.erase(body); }

static Atom* FindAtomById(bc::list<Atom*>& gos, unsigned long id)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		if (atom->id.id == id) return atom;
		if (!atom->children.empty())
			if (Atom* a = FindAtomById(atom->children, id)) return a;
	}
	return nullptr;
}

// Hit-atom resolution shared by every cast: colliders first, then the external registry.
static Atom* AtomOfBody(World* w, uint64_t body)
{
	if (!w) return nullptr;
	if (Collider* col = FindColliderByBody(w->GetHierarchy(), body)) return col->atom;
	auto it = g_extBody.find(body);
	return it == g_extBody.end() ? nullptr : FindAtomById(w->GetHierarchy(), it->second);
}

bool Physics::Available() { return GetService<iPhysics>() != nullptr; }

bool Physics::Raycast(const Vector3& from, const Vector3& dir, double maxDist)
{
	tl_lastHit = RayHit{};
	tl_uvDone = false; tl_uv = Vector2();
	iPhysics* p = GetService<iPhysics>();
	World* w = AppInstance::GetSingleton()->currentWorld;
	if (!p || !w) return false;

	float f[3] = { (float)from.x, (float)from.y, (float)from.z };
	float d[3] = { (float)dir.x,  (float)dir.y,  (float)dir.z };
	uint64_t body = 0;
	float point[3], normal[3];
	if (!p->raycast(f, d, (float)maxDist, body, point, normal)) return false;

	tl_lastHit.atom   = AtomOfBody(w, body);
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
	tl_uvDone = false; tl_uv = Vector2();
	iPhysics* p = GetService<iPhysics>();
	World* w = AppInstance::GetSingleton()->currentWorld;
	if (!p || !w) return false;

	const uint64_t ignoreBody = BodyOfAtom(p, ignore);
	float f[3] = { (float)from.x, (float)from.y, (float)from.z };
	float d[3] = { (float)dir.x,  (float)dir.y,  (float)dir.z };
	uint64_t body = 0;
	float point[3], normal[3];
	if (!p->raycastIgnore(f, d, (float)maxDist, ignoreBody, body, point, normal)) return false;

	tl_lastHit.atom   = AtomOfBody(w, body);
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
	tl_uvDone = false; tl_uv = Vector2();
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
	tl_lastHit.atom   = AtomOfBody(w, body);
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
	tl_uvDone = false; tl_uv = Vector2();
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

bool Physics::MeshUVAt(Atom* a, const Vector3& point, const Vector3& normal, float& u, float& v)
{
	u = v = 0.0f;
	if (!a) return false;
	// The atom's render mesh (colliders approximate it; a physics point sits on the collider,
	// not the surface): probe a short ray through the point ALONG -normal and take the mesh
	// triangle it crosses.
	Mesh* mesh = nullptr;
	for (Component* c : a->components)
	{
		if (!c || (std::strcmp(c->name, "MeshRenderer") != 0 && std::strcmp(c->name, "SkinnedMeshRenderer") != 0)) continue;
		if (((MeshRenderer*)c)->mesh) { mesh = ((MeshRenderer*)c)->mesh; break; }
	}
	if (!mesh) return false;
	Transform& t = a->GetTransform();
	const Vector3 P = t.globalPosition(); const Quaternion Q = t.globalRotation(); const Vector3 S = t.globalScale();
	const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3((float)P.x, (float)P.y, (float)P.z))
	                      * glm::mat4_cast(glm::quat((float)Q.w, (float)Q.x, (float)Q.y, (float)Q.z))
	                      * glm::scale(glm::mat4(1.0f), glm::vec3((float)S.x, (float)S.y, (float)S.z));
	const glm::mat4 inv = glm::inverse(world);
	// World-space probe: start slightly OUTSIDE the surface, aim through it.
	Vector3 n = normal;
	{ const double L = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z); if (L > 1e-12) { n.x /= L; n.y /= L; n.z /= L; } else n = Vector3(0, 1, 0); }
	const glm::vec4 lo = inv * glm::vec4((float)(point.x + n.x * 0.01),
	                                     (float)(point.y + n.y * 0.01),
	                                     (float)(point.z + n.z * 0.01), 1.0f);
	const glm::vec4 ld = inv * glm::vec4((float)-n.x, (float)-n.y, (float)-n.z, 0.0f);
	return mesh->RaycastUV(Vector3(lo.x, lo.y, lo.z), Vector3(ld.x, ld.y, ld.z), u, v) >= 0.0f;
}

Vector2 Physics::HitUV()
{
	if (tl_uvDone) return tl_uv;
	tl_uvDone = true; tl_uv = Vector2();
	float u = 0, v = 0;
	if (MeshUVAt(tl_lastHit.atom, tl_lastHit.point, tl_lastHit.normal, u, v))
		tl_uv = Vector2(u, v);
	return tl_uv;
}

const RayHit& Physics::LastHit() { return tl_lastHit; }

}  // namespace nuke
