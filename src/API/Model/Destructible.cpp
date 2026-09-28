// P2 destruction: Voronoi pieces (baked or cached), swap-on-break into piece atoms, a structure
// graph for what stays standing, debris budgets, "destruct.break" on the bus. See Destructible.h.
#include "API/Model/Destructible.h"
#include "API/Model/Atom.h"
#include "API/Model/Collider.h"
#include "API/Model/Rigidbody.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Mesh.h"
#include "API/Model/Material.h"
#include "API/Model/Fracture.h"
#include "API/Model/Events.h"
#include "API/Model/Prefab.h"
#include "API/Model/Physics.h"
#include "API/Model/World.h"
#include "API/Model/resdb.h"
#include "API/Model/Rand.h"
#include "service/iPhysics.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <functional>
#include <boost/container/list.hpp>

namespace nuke {

namespace {
	std::vector<Destructible*> gAll;      // game thread
	std::vector<DebrisPiece*>  gDebris;   // every piece marker alive (held + flying)
	int gMaxDebris = 0;                   // global cap on flying pieces (0 = none)
	struct Kick { long atom; Vector3 impulse; };
	std::vector<Kick> gKicks;             // impulses waiting for the piece's body to exist

	Destructible* FindOwner(long id)
	{
		for (Destructible* d : gAll) if (d && d->atom && (long)d->atom->id.id == id) return d;
		return nullptr;
	}
	Mesh* SourceMesh(Atom* a)
	{
		MeshRenderer* mr = a ? a->GetComponent<MeshRenderer>() : nullptr;
		if (!mr || mr->meshGuid.empty()) return nullptr;
		if (!mr->mesh) mr->mesh = ResDB::getSingleton()->GetMesh(mr->meshGuid);
		return mr->mesh;
	}
	double Len(const Vector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
	// The piece's tight bounds from its vertices (centroid-relative): the cut's halfExtents are loose.
	void PieceBox(const FracturePiece& fp, Vector3& mn, Vector3& mx)
	{
		mn = Vector3(1e30, 1e30, 1e30); mx = Vector3(-1e30, -1e30, -1e30);
		for (size_t k = 0; k + 2 < fp.verts.size(); k += 3)
		{
			mn.x = std::min(mn.x, (double)fp.verts[k]); mx.x = std::max(mx.x, (double)fp.verts[k]);
			mn.y = std::min(mn.y, (double)fp.verts[k + 1]); mx.y = std::max(mx.y, (double)fp.verts[k + 1]);
			mn.z = std::min(mn.z, (double)fp.verts[k + 2]); mx.z = std::max(mx.z, (double)fp.verts[k + 2]);
		}
		if (mn.x > mx.x) { mn = Vector3(0, 0, 0); mx = Vector3(0, 0, 0); }
	}
	Vector3 Scaled(const Vector3& v, const Vector3& s) { return Vector3(v.x * s.x, v.y * s.y, v.z * s.z); }
}

std::vector<Destructible::PendingBreak> Destructible::gPending;

// ---- lifecycle ------------------------------------------------------------------------------

Destructible::Destructible() : Component("Destructible") {}
void Destructible::Init(Atom* parent)
{
	atom = parent; transform = &parent->GetTransform();
	parent->components.push_back(this);
	if (std::find(gAll.begin(), gAll.end(), this) == gAll.end()) gAll.push_back(this);
}
void Destructible::Destroy() { gAll.erase(std::remove(gAll.begin(), gAll.end(), this), gAll.end()); }
void Destructible::Reset()   { adjacency.clear(); }

DebrisPiece::DebrisPiece() : Component("DebrisPiece") {}
void DebrisPiece::Init(Atom* parent)
{
	atom = parent; transform = &parent->GetTransform();
	parent->components.push_back(this);
	if (std::find(gDebris.begin(), gDebris.end(), this) == gDebris.end()) gDebris.push_back(this);
}
void DebrisPiece::Destroy() { gDebris.erase(std::remove(gDebris.begin(), gDebris.end(), this), gDebris.end()); }

// ---- script surface -------------------------------------------------------------------------

double Destructible::PieceCount()
{
	Mesh* m = SourceMesh(atom);
	const std::vector<FracturePiece>* ps = m ? FracturePieces(m, pieces, (uint32_t)seed) : nullptr;
	return ps ? (double)ps->size() : 0.0;
}
double Destructible::HeldCount()
{
	int n = 0;
	for (int id : heldPieces) if (id) ++n;
	return n;
}
double Destructible::DebrisCount()
{
	int n = 0;
	for (DebrisPiece* d : gDebris) if (d && d->flying) ++n;
	return n;
}
void Destructible::SetMaxDebris(double n) { gMaxDebris = (int)std::max(0.0, n); }

bool Destructible::Break(const Vector3& worldPoint, double radius)
{
	if (!atom || !SourceMesh(atom)) return false;
	gPending.push_back({ (long)atom->id.id, worldPoint, Vector3(0, 1, 0), radius < 0.0 ? (double)impactRadius : radius, false });
	return true;
}
bool Destructible::Shatter()
{
	if (!atom || !SourceMesh(atom)) return false;
	gPending.push_back({ (long)atom->id.id, transform ? transform->globalPosition() : Vector3(), Vector3(0, 1, 0), 0.0, true });
	return true;
}

// ---- engine plumbing ------------------------------------------------------------------------

Destructible* Destructible::Of(Atom* a)
{
	if (!a) return nullptr;
	if (Destructible* d = a->GetComponent<Destructible>()) return d;
	if (DebrisPiece* dp = a->GetComponent<DebrisPiece>()) return dp->flying ? nullptr : FindOwner(dp->owner);
	return nullptr;
}

void Destructible::Contact(Atom* self, Atom* other, float speed, const float point[3], const float normal[3])
{
	Destructible* d = Of(self);
	if (!d || !d->enabled) return;
	if (speed < d->breakSpeed) return;   // the closing speed at the moment of contact (the provider measures it before the solve)
	for (const PendingBreak& pb : gPending) if (pb.owner == (long)d->atom->id.id) return;   // one break per frame per structure
	gPending.push_back({ (long)d->atom->id.id, Vector3(point[0], point[1], point[2]), Vector3(normal[0], normal[1], normal[2]), (double)d->impactRadius, false });
	(void)other;
}

void Destructible::Drain(World* w)
{
	if (!w || gPending.empty()) return;
	std::vector<PendingBreak> take; take.swap(gPending);
	for (const PendingBreak& pb : take)
		if (Destructible* d = FindOwner(pb.owner)) d->DoBreak(w, pb.point, pb.normal, pb.radius, pb.all);
}

bool Destructible::SpawnPieces(World* w)
{
	Mesh* src = SourceMesh(atom);
	const std::vector<FracturePiece>* ps = src ? FracturePieces(src, pieces, (uint32_t)seed) : nullptr;
	if (!ps || ps->empty()) return false;
	MeshRenderer* mr = atom->GetComponent<MeshRenderer>();
	Collider* col = atom->GetComponent<Collider>();
	srcPos = transform->globalPosition(); srcRot = transform->globalRotation(); srcScale = transform->globalScale();
	const std::string inside = (mr && mr->mat) ? mr->mat->liveInsideMat : std::string();
	const int n = (int)ps->size();
	heldPieces.assign(n, 0);
	for (int i = 0; i < n; ++i)
	{
		const FracturePiece& fp = (*ps)[i];
		char nm[96]; std::snprintf(nm, sizeof(nm), "%s piece %d", atom->name.c_str(), i);
		Atom* pa = new Atom(nm);
		pa->GetTransform().SetGlobal(srcPos + srcRot.Rotate(Scaled(fp.centroid, srcScale)), srcRot, srcScale);
		MeshRenderer* pmr = new MeshRenderer();
		pmr->meshGuid = mr->meshGuid; pmr->fracturePiece = i; pmr->fracturePieces = pieces; pmr->fractureSeed = seed;
		pmr->matGuid = mr->matGuid;
		pmr->matGuids = { mr->matGuid, inside.empty() ? mr->matGuid : inside };   // surface / cut caps
		pa->AddComponent(pmr);
		Collider* pc = new Collider();
		pc->shape = Collider::S_Mesh; pc->convex = true;
		if (col) { pc->friction = col->friction; pc->restitution = col->restitution; }
		pa->AddComponent(pc);
		DebrisPiece* dp = new DebrisPiece();
		dp->owner = (int)atom->id.id; dp->piece = i; dp->flying = false;
		pa->AddComponent(dp);
		w->Add(pa);
		heldPieces[i] = (int)pa->id.id;
	}
	// The whole is gone: its draw and its body (the driver drops a disabled collider's body).
	if (mr) mr->enabled = false;
	if (col) col->enabled = false;
	return true;
}

// Neighbours = pieces whose bounds touch (Voronoi cells share a face); 3% margin.
static void BuildAdjacency(const std::vector<FracturePiece>& ps, std::vector<std::vector<int>>& adj)
{
	const int n = (int)ps.size();
	adj.assign(n, {});
	for (int i = 0; i < n; ++i)
		for (int j = i + 1; j < n; ++j)
		{
			const FracturePiece& a = ps[i]; const FracturePiece& b = ps[j];
			bool touch = true;
			Vector3 amn, amx, bmn, bmx; PieceBox(a, amn, amx); PieceBox(b, bmn, bmx);
			const double ac[3] = { a.centroid.x + (amn.x + amx.x) * 0.5, a.centroid.y + (amn.y + amx.y) * 0.5, a.centroid.z + (amn.z + amx.z) * 0.5 };
			const double ah[3] = { (amx.x - amn.x) * 0.5, (amx.y - amn.y) * 0.5, (amx.z - amn.z) * 0.5 };
			const double bc[3] = { b.centroid.x + (bmn.x + bmx.x) * 0.5, b.centroid.y + (bmn.y + bmx.y) * 0.5, b.centroid.z + (bmn.z + bmx.z) * 0.5 };
			const double bh[3] = { (bmx.x - bmn.x) * 0.5, (bmx.y - bmn.y) * 0.5, (bmx.z - bmn.z) * 0.5 };
			for (int k = 0; k < 3 && touch; ++k)
			{
				const double margin = 0.03 * (ah[k] + bh[k]) + 1e-3;
				if (std::fabs(ac[k] - bc[k]) > ah[k] + bh[k] + margin) touch = false;
			}
			if (touch) { adj[i].push_back(j); adj[j].push_back(i); }
		}
}

void Destructible::DoBreak(World* w, const Vector3& point, const Vector3& normal, double radius, bool all)
{
	Mesh* src = SourceMesh(atom);
	const std::vector<FracturePiece>* ps = src ? FracturePieces(src, pieces, (uint32_t)seed) : nullptr;
	if (!ps) return;
	if (!broken)
	{
		if (!SpawnPieces(w)) return;
		broken = true;
	}
	else if (heldPieces.size() != ps->size()) heldPieces.resize(ps->size(), 0);   // a loaded save
	if (adjacency.size() != ps->size())
	{
		BuildAdjacency(*ps, adjacency);
		srcPos = transform->globalPosition(); srcRot = transform->globalRotation(); srcScale = transform->globalScale();
	}
	// A dynamic destructible has nothing to stand on: every piece flies; so does a non-structural one.
	if (!structural || atom->GetComponent<Rigidbody>()) all = true;
	std::vector<int> det; int nearest = -1; double best = 1e30;
	for (int i = 0; i < (int)ps->size(); ++i)
	{
		if (!heldPieces[i]) continue;
		Vector3 c = srcPos + srcRot.Rotate(Scaled((*ps)[i].centroid, srcScale)), pp = point;
		const double dist = Len(c - pp);
		if (all || dist <= radius) det.push_back(i);
		if (dist < best) { best = dist; nearest = i; }
	}
	if (det.empty() && nearest >= 0) det.push_back(nearest);
	for (int i : det) Detach(w, i, point, normal, true);
	if (structural && !all) Collapse(w, Physics::Scene());
	// the material's debris prefabs (dust, splinters) at the hit, as the fire system throws them
	if (MeshRenderer* mr = atom->GetComponent<MeshRenderer>())
		if (mr->mat && !mr->mat->liveDebrisPrefab.empty())
			for (int k = 0; k < std::max(1, mr->mat->liveDebrisCount); ++k)
				if (Atom* dbr = Prefabs::SpawnIn(w, mr->mat->liveDebrisPrefab))
				{
					Transform& t = dbr->GetTransform();
					t.SetGlobal(point, t.globalRotation(), t.globalScale());
					if (Rigidbody* rb = dbr->GetComponent<Rigidbody>())
						gKicks.push_back({ (long)dbr->id.id, Vector3(Rand::Range("destruct", -1.0, 1.0), Rand::Range("destruct", 0.5, 1.5), Rand::Range("destruct", -1.0, 1.0)) * (double)(rb->mass * 2.0f) });
				}
	nlohmann::json j;
	j["atom"] = atom->name; j["id"] = (double)atom->id.id;
	j["x"] = point.x; j["y"] = point.y; j["z"] = point.z;
	j["nx"] = normal.x; j["ny"] = normal.y; j["nz"] = normal.z;
	j["pieces"] = (int)det.size(); j["held"] = (int)HeldCount(); j["total"] = (int)ps->size();
	Events::EmitEngine("destruct.break", j.dump());
}

void Destructible::Detach(World* w, int index, const Vector3& point, const Vector3& normal, bool kickIt)
{
	if (index < 0 || index >= (int)heldPieces.size() || !heldPieces[index]) return;
	Atom* pa = w->GetById(heldPieces[index]);
	heldPieces[index] = 0;
	if (!pa) return;
	if (DebrisPiece* dp = pa->GetComponent<DebrisPiece>()) { dp->flying = true; dp->age = 0.f; dp->still = 0.f; }
	Mesh* src = SourceMesh(atom);
	const std::vector<FracturePiece>* ps = src ? FracturePieces(src, pieces, (uint32_t)seed) : nullptr;
	Vector3 he = ps && index < (int)ps->size() ? Scaled((*ps)[index].halfExtents, srcScale) : Vector3(0.1, 0.1, 0.1);
	Rigidbody* rb = new Rigidbody();
	const double vol = 8.0 * std::fabs(he.x * he.y * he.z);
	rb->mass = (float)std::min(100.0, std::max(0.2, vol * 300.0));
	pa->AddComponent(rb);   // the driver creates the dynamic body next fixed step; the kick waits for it
	if (kickIt && kick > 0.f)
	{
		Vector3 dir = pa->GetTransform().globalPosition() - point;
		const double dl = Len(dir);
		dir = dl > 1e-4 ? dir * (1.0 / dl) : Vector3(0, 1, 0);
		Vector3 n = normal; const double nn = Len(n); if (nn > 1e-4) n = n * (1.0 / nn); else n = Vector3(0, 0, 0);
		const Vector3 imp = (dir * 0.7 + n * 0.3 + Vector3(0, 0.2, 0)) * (double)(rb->mass * kick * 2.0f);
		gKicks.push_back({ (long)pa->id.id, imp });
	}
}

// Supports = held pieces touching something that is not a piece of this structure (ground,
// walls); pieces that no support reaches through the neighbour graph fall.
void Destructible::Collapse(World* w, iPhysics* p)
{
	if (!p) return;
	Mesh* src = SourceMesh(atom);
	const std::vector<FracturePiece>* ps = src ? FracturePieces(src, pieces, (uint32_t)seed) : nullptr;
	if (!ps) return;
	const int n = (int)ps->size();
	// What can hold a piece up: static or kinematic bodies that are not this structure's own. A ball
	// leaning on a wall holds nothing - dynamic bodies are ignored.
	std::set<uint64_t> ours;
	std::function<void(bc::list<Atom*>&)> scan = [&](bc::list<Atom*>& list)
	{
		for (Atom* a : list)
		{
			if (!a) continue;
			if (Collider* c = a->GetComponent<Collider>())
				if (c->bodyId)
				{
					Rigidbody* rb = a->GetComponent<Rigidbody>();
					DebrisPiece* dp = a->GetComponent<DebrisPiece>();
					if (a == atom || (dp && dp->owner == (int)atom->id.id) || (rb && !rb->isKinematic)) ours.insert(c->bodyId);
				}
			if (!a->children.empty()) scan(a->children);
		}
	};
	scan(w->GetHierarchy());
	std::vector<char> supported(n, 0), held(n, 0);
	for (int i = 0; i < n; ++i)
	{
		if (!heldPieces[i]) continue;
		held[i] = 1;
		const FracturePiece& fp = (*ps)[i];
		Vector3 bmn, bmx; PieceBox(fp, bmn, bmx);
		const Vector3 lc = Vector3(fp.centroid.x + (bmn.x + bmx.x) * 0.5, fp.centroid.y + (bmn.y + bmx.y) * 0.5, fp.centroid.z + (bmn.z + bmx.z) * 0.5);
		const Vector3 c = srcPos + srcRot.Rotate(Scaled(lc, srcScale));
		const Vector3 he = Scaled(Vector3((bmx.x - bmn.x) * 0.5, (bmx.y - bmn.y) * 0.5, (bmx.z - bmn.z) * 0.5), srcScale);
		NukeShapeDesc sh; sh.shape = 0;
		sh.halfExtents[0] = (float)std::fabs(he.x) + 0.02f; sh.halfExtents[1] = (float)std::fabs(he.y) + 0.02f; sh.halfExtents[2] = (float)std::fabs(he.z) + 0.02f;
		const float pos[3] = { (float)c.x, (float)c.y, (float)c.z };
		const float q[4] = { (float)srcRot.x, (float)srcRot.y, (float)srcRot.z, (float)srcRot.w };
		uint64_t bodies[32];
		const int hit = p->overlap(sh, pos, q, bodies, 32);
		for (int k = 0; k < hit; ++k) if (!ours.count(bodies[k])) { supported[i] = 1; break; }
		if (std::getenv("NUKE_DESTRUCT_DEBUG"))
		{
			std::cout << "[Destruct]	" << atom->name << " piece " << i << " y " << c.y - he.y << ".." << c.y + he.y << " bodies " << hit << ":";
			for (int k = 0; k < hit; ++k) std::cout << " " << bodies[k] << (ours.count(bodies[k]) ? "(ours)" : "(SUPPORT)");
			std::cout << std::endl;
		}
	}
	// flood from the supports over the neighbour graph
	std::vector<char> reached(n, 0); std::vector<int> stack;
	for (int i = 0; i < n; ++i) if (held[i] && supported[i]) { reached[i] = 1; stack.push_back(i); }
	while (!stack.empty())
	{
		const int i = stack.back(); stack.pop_back();
		for (int j : adjacency[i]) if (held[j] && !reached[j]) { reached[j] = 1; stack.push_back(j); }
	}
	for (int i = 0; i < n; ++i) if (held[i] && !reached[i]) Detach(w, i, srcPos, Vector3(0, 1, 0), false);
}

void Destructible::Tick(World* w, float dt)
{
	if (!w) return;
	// kicks land once the body exists
	for (size_t i = 0; i < gKicks.size(); )
	{
		Atom* a = w->GetById(gKicks[i].atom);
		Collider* c = a ? a->GetComponent<Collider>() : nullptr;
		Rigidbody* rb = a ? a->GetComponent<Rigidbody>() : nullptr;
		if (!a) { gKicks.erase(gKicks.begin() + i); continue; }
		if (c && c->bodyId && rb) { rb->AddImpulse(gKicks[i].impulse); gKicks.erase(gKicks.begin() + i); continue; }
		++i;
	}
	if (gDebris.empty()) return;
	// budgets: lifetime, lying still, per-owner and global counts (oldest first)
	std::vector<long> kill;
	struct Alive { DebrisPiece* d; Destructible* o; };
	std::vector<Alive> flying;
	for (DebrisPiece* d : gDebris)
	{
		if (!d || !d->flying || !d->atom) continue;
		d->age += dt;
		Rigidbody* rb = d->atom->GetComponent<Rigidbody>();
		const double v = rb ? Len(rb->Velocity()) : 0.0;
		d->still = v < 0.05 ? d->still + dt : 0.f;
		Destructible* o = FindOwner(d->owner);
		const float life = o ? o->debrisLifetime : 30.f, sleep = o ? o->debrisSleep : 8.f;
		if ((life > 0.f && d->age > life) || (sleep > 0.f && d->still > sleep)) { kill.push_back((long)d->atom->id.id); continue; }
		flying.push_back({ d, o });
	}
	std::sort(flying.begin(), flying.end(), [](const Alive& a, const Alive& b) { return a.d->age > b.d->age; });   // oldest first
	std::map<Destructible*, int> perOwner;
	int total = 0;
	for (const Alive& a : flying)
	{
		++total;
		int& cnt = perOwner[a.o]; ++cnt;
		const int mx = a.o ? a.o->debrisMax : 0;
		(void)mx;
	}
	// over the caps: the oldest go
	std::map<Destructible*, int> seen;
	int seenTotal = 0;
	for (const Alive& a : flying)
	{
		const int mx = a.o ? a.o->debrisMax : 0;
		const int cnt = perOwner[a.o];
		int& s = seen[a.o];
		const bool overOwner = mx > 0 && cnt - s > mx;
		const bool overGlobal = gMaxDebris > 0 && total - seenTotal > gMaxDebris;
		if (overOwner || overGlobal) { kill.push_back((long)a.d->atom->id.id); --perOwner[a.o]; --total; continue; }
		++s; ++seenTotal;
	}
	for (long id : kill) w->RemoveAtomById(id);
}

}  // namespace nuke
