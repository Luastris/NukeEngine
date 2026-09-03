// Fire system: LiveMaterial-driven ignition/spread/burn-out + debris destruction (see Fire.h).
#include "API/Model/Physics.h"
#include "API/Model/Fire.h"
#include "API/Model/Atom.h"
#include "API/Model/Collider.h"
#include "API/Model/Events.h"
#include "API/Model/Fracture.h"
#include "API/Model/Game.h"
#include "API/Model/Material.h"
#include "API/Model/Mesh.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Prefab.h"
#include "API/Model/Rigidbody.h"
#include "API/Model/Surface.h"
#include "API/Model/Time.h"
#include "API/Model/World.h"
#include "interface/AppInstance.h"
#include "interface/Services.h"
#include "service/iPhysics.h"
#include "render/irender.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>


namespace nuke {

namespace {

bool   g_enabled = true;
int    g_maxFires = 64;
double g_lastTick = -1.0;
double g_scanIn = 0.0;      // seconds until the flammable-candidate rescan
double g_spreadIn = 0.0;    // seconds until the next heat-spread pass
std::vector<FireState*> g_states;
std::vector<long> g_candidates;      // atom ids with a flammable material
struct PendingIgnite { long id = 0; Vector3 pos; bool hasPos = false; };
std::vector<PendingIgnite> g_pendingIgnite;   // queued from scripts, applied post-traversal
std::vector<long> g_pendingShatter;           // same rule (spawns/destroys happen in Tick)

// Deferred prefab spawns, performed by Drain on the render phase (a spawn from the game tick
// races the render pass — the house rule since Surface hits).
struct PendingSpawn
{
	std::string prefab;
	long owner = 0;        // fire visual: the burning atom (parent + FireState::fx)
	Vector3 pos;           // debris: world drop point
	Vector3 impulse;
	bool debris = false;
};
std::vector<PendingSpawn> g_pendingSpawns;

// A queued REAL fracture: the source mesh splits into Voronoi fragments in Drain.
struct PendingFracture
{
	const Mesh* mesh = nullptr;   // asset mesh (ResDB-owned, outlives the atom)
	std::string matGuid;
	std::string insideMat;        // cut faces draw this (empty = the surface material)
	Vector3 pos, scale;
	Quaternion rot;
	int count = 8;
	uint32_t seed = 1;
};
std::vector<PendingFracture> g_pendingFractures;
std::vector<std::pair<long, Mesh*>> g_pieceMeshes;      // piece atom id -> OWNED fragment mesh
std::vector<std::pair<long, Vector3>> g_pieceImpulses;  // applied once the piece body exists

Material* FlamMat(Atom* a)
{
	MeshRenderer* mr = a ? a->GetComponent<MeshRenderer>() : nullptr;
	Material* m = mr ? mr->mat : nullptr;
	return (m && m->liveIgnite >= 0.0f) ? m : nullptr;
}

FireState* StateOf(Atom* a)
{
	return a ? a->GetComponent<FireState>() : nullptr;
}

FireState* EnsureState(Atom* a)
{
	if (FireState* s = StateOf(a)) return s;
	FireState* s = new FireState();
	s->Init(a);
	return s;
}

void EmitFire(const char* ev, Atom* a)
{
	nlohmann::json j;
	j["atom"] = (double)(a ? a->id.id : 0);
	Events::Emit(ev, j.dump());
}

int BurningCount()
{
	int n = 0;
	for (FireState* s : g_states) if (s->burning) ++n;
	return n;
}

// The nearest point ON the atom's mesh bounds to a world point: the fire catches on a face
// of the object, never in the air between it and the heat source.
Vector3 SnapToBounds(Atom* a, const Vector3& worldP)
{
	MeshRenderer* mr = a->GetComponent<MeshRenderer>();
	if (!mr || !mr->mesh) return worldP;
	Mesh* msh = mr->mesh;
	msh->EnsureBounds();
	Transform& t = a->GetTransform();
	Quaternion gr = t.globalRotation();
	Vector3 gs = t.globalScale();
	Vector3 wp = worldP;   // Vector3 arithmetic is non-const
	Vector3 lp = gr.conjugate().Rotate(wp - t.globalPosition());
	lp = Vector3(gs.x != 0.0 ? lp.x / gs.x : lp.x,
	             gs.y != 0.0 ? lp.y / gs.y : lp.y,
	             gs.z != 0.0 ? lp.z / gs.z : lp.z);
	lp.x = std::min((double)msh->aabbMax[0], std::max((double)msh->aabbMin[0], lp.x));
	lp.y = std::min((double)msh->aabbMax[1], std::max((double)msh->aabbMin[1], lp.y));
	lp.z = std::min((double)msh->aabbMax[2], std::max((double)msh->aabbMin[2], lp.z));
	return t.globalPosition() + gr.Rotate(Vector3(lp.x * gs.x, lp.y * gs.y, lp.z * gs.z));
}

// The atom's "burn" mask (any authored one carrying the state, else our transient own): the
// fire FRONT paints into it, so the char spreads over the mesh from the ignition point.
SurfaceMask* EnsureBurnMask(Atom* a)
{
	for (Component* c : a->components)
		if (SurfaceMask* mk = dynamic_cast<SurfaceMask*>(c))
			if (mk->StateSlot("burn") >= 0) return mk;
	SurfaceMask* mk = new SurfaceMask();
	mk->state0 = "burn";
	mk->resolution = 12;
	Vector3 he(1, 1, 1);
	if (MeshRenderer* mr = a->GetComponent<MeshRenderer>())
		if (mr->mesh)
		{
			mr->mesh->EnsureBounds();
			he = Vector3((mr->mesh->aabbMax[0] - mr->mesh->aabbMin[0]) * 0.5 + 0.1,
			             (mr->mesh->aabbMax[1] - mr->mesh->aabbMin[1]) * 0.5 + 0.1,
			             (mr->mesh->aabbMax[2] - mr->mesh->aabbMin[2]) * 0.5 + 0.1);
		}
	mk->halfExtents = he;
	mk->transient = true;   // repainted every tick from ignitePos + progress; never serialized
	a->AddComponent(mk);
	return mk;
}

// Start burning: state flip + the material's looping fire visual (spawned by Drain).
void StartBurn(World* w, Atom* a, FireState* s, Material* m, const Vector3& ignitePos)
{
	(void)w;
	s->burning = true;
	s->burnT = std::max(0.0f, s->burnT);
	s->ignitePos = ignitePos;
	// A material with no authored "burn" response would char invisibly — inject a default
	// blackening state into THIS instance (authors override by adding their own "burn" state).
	bool hasBurn = false;
	for (const LiveState& ls : m->liveStates)
		if (ls.state == "burn") { hasBurn = true; break; }
	if (!hasBurn)
	{
		LiveState ls;
		ls.state = "burn";
		ls.color = Color(0.06, 0.05, 0.045, 1.0);
		ls.roughness = 0.95f;
		ls.threshold = 0.0f;
		ls.feather = 1.0f;
		m->liveStates.push_back(ls);
		m->Resolve();
	}
	if (!s->fx && !m->liveFirePrefab.empty())
	{
		PendingSpawn ps;
		ps.prefab = m->liveFirePrefab;
		ps.owner = a->id.id;
		g_pendingSpawns.push_back(ps);
	}
	EmitFire("fire.ignite", a);
}

void StopFx(World* w, FireState* s)
{
	if (s->fx && w) w->QueueDestroy(s->fx->id.id);
	s->fx = nullptr;
}

// One-shot guard shared by burn-out and Shatter (both may land in one tick).
bool MarkShattered(Atom* a)
{
	FireState* s = StateOf(a);
	if (s && s->shattered) return false;
	if (s) s->shattered = true;
	return true;
}

// The material's debris prefabs thrown from the atom's position (burn-out embers, splinters).
void SpawnDebrisPrefabs(Atom* a, Material* m)
{
	if (!m || m->liveDebrisPrefab.empty()) return;
	Vector3 p = a->GetTransform().globalPosition();
	const int n = std::max(1, m->liveDebrisCount);
	for (int i = 0; i < n; ++i)
	{
		const double ang = (double)i / n * 6.283185307179586;
		PendingSpawn ps;
		ps.prefab = m->liveDebrisPrefab;
		ps.debris = true;
		ps.pos = p + Vector3(std::cos(ang) * 0.2, 0.1 * i, std::sin(ang) * 0.2);
		ps.impulse = Vector3(std::cos(ang) * 2.0, 2.0 + 0.5 * (i % 3), std::sin(ang) * 2.0);
		g_pendingSpawns.push_back(ps);
	}
}

// Burn-out destruction: charred remains crumble into the debris PREFABS (embers, ash chunks)
// — burnt wood does not shatter like glass; the Voronoi fracture belongs to Destruct.Shatter.
void BurnDebris(World* w, Atom* a, Material* m)
{
	if (!MarkShattered(a)) return;
	SpawnDebrisPrefabs(a, m);
	w->QueueDestroy(a->id.id);
}

// Game-called shatter: fracture the REAL mesh into Voronoi fragments (brittle stuff — glass,
// stone, pottery; the game's damage system decides), plus the prefab dressing if authored.
void ShatterAtom(World* w, Atom* a, Material* m)
{
	if (!MarkShattered(a)) return;
	MeshRenderer* mr = a->GetComponent<MeshRenderer>();
	// Asset meshes outlive the atom (ResDB); a transient generated mesh dies with it, so only
	// the prefab dressing remains for those.
	if (mr && mr->mesh && !mr->mesh->guid.empty())
	{
		PendingFracture pf;
		pf.mesh = mr->mesh;
		pf.matGuid = mr->matGuid;
		pf.insideMat = m ? m->liveInsideMat : std::string();
		pf.pos = a->GetTransform().globalPosition();
		pf.rot = a->GetTransform().globalRotation();
		pf.scale = a->GetTransform().globalScale();
		pf.count = m ? std::max(2, m->liveDebrisCount) : 8;
		pf.seed = (uint32_t)a->id.id * 2654435761u + 12345u;
		g_pendingFractures.push_back(pf);
	}
	SpawnDebrisPrefabs(a, m);
	w->QueueDestroy(a->id.id);
}

// Burn-out: full char stays; a debris prefab shatters the atom into bodies and removes it.
void BurnOut(World* w, Atom* a, FireState* s, Material* m)
{
	s->burning = false;
	s->burned = true;
	StopFx(w, s);
	EmitFire("fire.out", a);
	if (!m->liveDebrisPrefab.empty())
	{
		EmitFire("fire.destroyed", a);
		BurnDebris(w, a, m);
	}
}

// The atom's material when it carries debris data (shatterable).
Material* ShatterMat(Atom* a)
{
	MeshRenderer* mr = a ? a->GetComponent<MeshRenderer>() : nullptr;
	Material* m = mr ? mr->mat : nullptr;
	return (m && !m->liveDebrisPrefab.empty()) ? m : nullptr;
}

// Shatterable: an asset mesh fractures for real; debris-prefab data alone also qualifies.
bool CanShatterAtom(Atom* a)
{
	MeshRenderer* mr = a ? a->GetComponent<MeshRenderer>() : nullptr;
	if (mr && mr->mesh && !mr->mesh->guid.empty()) return true;
	return ShatterMat(a) != nullptr;
}

// The char FRONT: a growing sphere painted into the burn mask from the ignition point.
void PaintCharFront(Atom* a, FireState* st, float progress, float dt)
{
	SurfaceMask* mk = EnsureBurnMask(a);
	const int slot = mk->StateSlot("burn");
	if (slot < 0) return;
	// Reach in WORLD meters: the mask box is local halfExtents x the atom's scale.
	Vector3 he = mk->halfExtents;
	Vector3 gs = a->GetTransform().globalScale();
	he = Vector3(he.x * std::abs(gs.x), he.y * std::abs(gs.y), he.z * std::abs(gs.z));
	const double maxReach = 2.2 * std::sqrt(he.x * he.x + he.y * he.y + he.z * he.z);
	const double radius = 0.15 + (double)progress * maxReach;
	// The brush falls off linearly to ZERO at its edge, so painting the bare front radius
	// leaves the rim unpainted forever — overshoot the brush and pump the amount: the
	// interior saturates fast, the rim keeps a moving char gradient.
	mk->Paint(st->ignitePos, radius * 1.35, slot, dt * 4.0);
}

}  // namespace

void FireState::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	Fire::Register(this);
}

void FireState::Destroy() { Fire::Unregister(this); }

void Fire::Drain(World* w)
{
	if (!w || (g_pendingSpawns.empty() && g_pendingFractures.empty())) return;

	// Real mesh fracture: build the Voronoi fragments and stand each one up as a physics atom.
	std::vector<PendingFracture> takeF;
	takeF.swap(g_pendingFractures);
	for (PendingFracture& pf : takeF)
	{
		std::vector<FracturePiece> pieces;
		if (!FractureMesh(pf.mesh, pf.scale, pf.count, pf.seed, pieces)) continue;
		for (FracturePiece& fp : pieces)
		{
			Mesh* pm = new Mesh();
			std::snprintf(pm->name, sizeof(pm->name), "fracture");
			pm->vertexArray = new float[fp.verts.size()];
			pm->normalArray = new float[fp.normals.size()];
			pm->uvArray = new float[fp.uvs.size()];
			std::memcpy(pm->vertexArray, fp.verts.data(), fp.verts.size() * sizeof(float));
			std::memcpy(pm->normalArray, fp.normals.data(), fp.normals.size() * sizeof(float));
			std::memcpy(pm->uvArray, fp.uvs.data(), fp.uvs.size() * sizeof(float));
			pm->numVerts = (int)(fp.verts.size() / 3);
			pm->numIndices = 0;
			pm->indexArray = nullptr;
			// The insides of a brick are not its glaze: cut caps get their own material slot.
			const bool split = !pf.insideMat.empty() &&
			                   fp.surfVerts > 0 && (size_t)pm->numVerts > fp.surfVerts;
			if (split)
			{
				pm->numIndices = pm->numVerts;
				pm->indexArray = new uint32_t[pm->numIndices];
				for (int i = 0; i < pm->numIndices; ++i) pm->indexArray[i] = (uint32_t)i;
				pm->sections = { { 0, (uint32_t)fp.surfVerts, 0 },
				                 { (uint32_t)fp.surfVerts,
				                   (uint32_t)(pm->numVerts - (int)fp.surfVerts), 1 } };
				pm->numSlots = 2;
			}
			pm->boundsValid = false;
			++pm->version;

			Atom* pa = new Atom("fracture");
			pa->GetTransform().SetGlobal(pf.pos + pf.rot.Rotate(fp.centroid), pf.rot, Vector3(1, 1, 1));
			MeshRenderer* mr = new MeshRenderer();
			mr->mesh = pm;
			mr->matGuid = pf.matGuid;
			if (split) mr->matGuids = { pf.matGuid, pf.insideMat };
			pa->AddComponent(mr);
			Collider* col = new Collider();
			col->shape = Collider::S_Mesh;
			col->convex = true;
			pa->AddComponent(col);
			Rigidbody* rb = new Rigidbody();
			const double vol = 8.0 * fp.halfExtents.x * fp.halfExtents.y * fp.halfExtents.z;
			rb->mass = (float)std::min(100.0, std::max(0.2, vol * 300.0));
			pa->AddComponent(rb);
			w->Add(pa);
			g_pieceMeshes.push_back({ pa->id.id, pm });
			Vector3 dir = fp.centroid;
			const double dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
			dir = dl > 1e-6 ? Vector3(dir.x / dl, dir.y / dl, dir.z / dl) : Vector3(0, 1, 0);
			g_pieceImpulses.push_back({ pa->id.id, pf.rot.Rotate(dir) * (double)(rb->mass * 2.0f) });
		}
	}

	if (g_pendingSpawns.empty()) return;
	std::vector<PendingSpawn> take;
	take.swap(g_pendingSpawns);
	for (PendingSpawn& ps : take)
	{
		if (ps.debris)
		{
			Atom* d = Prefabs::SpawnIn(w, ps.prefab);
			if (!d) continue;
			Transform& t = d->GetTransform();
			t.SetGlobal(ps.pos, t.globalRotation(), t.globalScale());
			if (Rigidbody* rb = d->GetComponent<Rigidbody>())
				rb->AddImpulse(ps.impulse);
		}
		else
		{
			// Fire visual: the owner may have died or been put out before the pump ran.
			Atom* a = w->GetById(ps.owner);
			FireState* s = a ? StateOf(a) : nullptr;
			if (!s || !s->burning || s->fx) continue;
			Atom* fx = Prefabs::SpawnIn(w, ps.prefab);
			if (!fx) continue;
			fx->SetParent(a);
			// Ride the burning atom AT the ignition point: world -> the atom's local frame.
			Transform& at = a->GetTransform();
			Quaternion gr = at.globalRotation();
			Vector3 rel = s->ignitePos - at.globalPosition();
			Vector3 lp = gr.conjugate().Rotate(rel);
			Vector3 gs = at.globalScale();
			lp = Vector3(gs.x != 0.0 ? lp.x / gs.x : lp.x,
			             gs.y != 0.0 ? lp.y / gs.y : lp.y,
			             gs.z != 0.0 ? lp.z / gs.z : lp.z);
			fx->GetTransform().position = lp;
			s->fx = fx;
		}
	}
}

// ---- damage destruction -------------------------------------------------------------------

// Queued: scripts call mid-traversal, spawns/destroys happen post-traversal (Tick).
bool Destruct::Shatter(Atom* a)
{
	if (!CanShatterAtom(a)) return false;
	g_pendingShatter.push_back(a->id.id);
	return true;
}

bool Destruct::CanShatter(Atom* a) { return CanShatterAtom(a); }

void Fire::Register(FireState* s)
{
	if (std::find(g_states.begin(), g_states.end(), s) == g_states.end())
		g_states.push_back(s);
}

void Fire::Unregister(FireState* s)
{
	g_states.erase(std::remove(g_states.begin(), g_states.end(), s), g_states.end());
}

bool Fire::Ignite(Atom* a)
{
	if (!a || !g_enabled || !FlamMat(a)) return false;
	if (BurningCount() >= g_maxFires) return false;
	g_pendingIgnite.push_back({ (long)a->id.id, Vector3(), false });
	return true;
}

bool Fire::IgnitePoint(Atom* a, const Vector3& worldPos)
{
	if (!a || !g_enabled || !FlamMat(a)) return false;
	if (BurningCount() >= g_maxFires) return false;
	g_pendingIgnite.push_back({ (long)a->id.id, worldPos, true });
	return true;
}

void Fire::IgniteAt(const Vector3& pos, double radius)
{
	World* w = Game::GetWorld();
	if (!w || !g_enabled || radius <= 0.0) return;
	Vector3 p = pos;
	for (long id : g_candidates)
	{
		Atom* a = w->GetById(id);
		if (!a || !a->enabled) continue;
		Vector3 ap = a->GetTransform().globalPosition();
		Vector3 d = ap - p;
		if (d.x * d.x + d.y * d.y + d.z * d.z <= radius * radius)
			IgnitePoint(a, SnapToBounds(a, p));   // the fire starts on the source-facing side
	}
}

void Fire::Extinguish(Atom* a)
{
	FireState* s = StateOf(a);
	if (!s || !s->burning) return;
	s->burning = false;
	StopFx(Game::GetWorld(), s);
	EmitFire("fire.out", a);
}

bool Fire::Burning(Atom* a)
{
	FireState* s = StateOf(a);
	return s && s->burning;
}

double Fire::BurnProgress(Atom* a)
{
	FireState* s = StateOf(a);
	if (!s) return -1.0;
	Material* m = FlamMat(a);
	const float dur = m ? std::max(0.1f, m->liveBurn) : 1.0f;
	return s->burned ? 1.0 : std::min(1.0f, s->burnT / dur);
}

double Fire::ActiveFires()      { return (double)BurningCount(); }
void   Fire::SetEnabled(bool on){ g_enabled = on; }
bool   Fire::Enabled()          { return g_enabled; }
void   Fire::SetMaxFires(double n) { g_maxFires = std::max(1, (int)n); }

void Fire::Tick(World* w)
{
	if (!w || !Game::IsPlaying()) { g_lastTick = -1.0; return; }
	const double now = Time::getSingleton()->elapsed;
	double dt = (g_lastTick < 0) ? 0.0 : (now - g_lastTick);
	g_lastTick = now;
	if (!g_enabled) return;
	dt = std::min(dt, 0.25);

	// Fracture-piece upkeep: launch impulses once the bodies exist, free meshes of dead pieces.
	if (iPhysics* ph = Physics::Scene())
		for (size_t i = g_pieceImpulses.size(); i-- > 0;)
		{
			Atom* a = w->GetById(g_pieceImpulses[i].first);
			Collider* c = a ? a->GetComponent<Collider>() : nullptr;
			if (a && (!c || !c->bodyId)) continue;   // body next fixed step
			if (c && c->bodyId)
			{
				const Vector3& v = g_pieceImpulses[i].second;
				const float imp[3] = { (float)v.x, (float)v.y, (float)v.z };
				ph->addImpulse(c->bodyId, imp);
			}
			g_pieceImpulses.erase(g_pieceImpulses.begin() + i);
		}
	for (size_t i = g_pieceMeshes.size(); i-- > 0;)
		if (!w->GetById(g_pieceMeshes[i].first))
		{
			Mesh* pm = g_pieceMeshes[i].second;
			if (AppInstance* app = AppInstance::GetSingleton())
				if (app->render) app->render->invalidateMesh(pm);
			delete[] pm->vertexArray; delete[] pm->normalArray; delete[] pm->uvArray;
			delete[] pm->indexArray;
			delete pm;
			g_pieceMeshes.erase(g_pieceMeshes.begin() + i);
		}

	// Flammable-candidate cache: the tag walk is the heavy part, so it runs on a timer.
	g_scanIn -= dt;
	if (g_scanIn <= 0.0)
	{
		g_scanIn = 1.0;
		g_candidates.clear();
		std::vector<Atom*> walk(w->GetHierarchy().begin(), w->GetHierarchy().end());
		while (!walk.empty())
		{
			Atom* a = walk.back(); walk.pop_back();
			if (!a) continue;
			for (Atom* c : a->children) walk.push_back(c);
			if (FlamMat(a)) g_candidates.push_back(a->id.id);
		}
	}

	// Queued ignitions (scripts call mid-traversal; components attach here, post-traversal).
	for (PendingIgnite& pi : g_pendingIgnite)
	{
		Atom* a = w->GetById(pi.id);
		Material* m = a ? FlamMat(a) : nullptr;
		if (!a || !m) continue;
		FireState* s = EnsureState(a);
		if (!s->burning && !s->burned)
			StartBurn(w, a, s, m, pi.hasPos ? pi.pos : a->GetTransform().globalPosition());
	}
	g_pendingIgnite.clear();

	// Queued shatters (the game's own damage system decided; we only execute).
	for (long id : g_pendingShatter)
	{
		Atom* a = w->GetById(id);
		if (!a || !CanShatterAtom(a)) continue;
		EnsureState(a);   // the double-shatter guard lives on the state
		EmitFire("destruct.shatter", a);
		MeshRenderer* mr = a->GetComponent<MeshRenderer>();
		ShatterAtom(w, a, mr ? mr->mat : nullptr);
	}
	g_pendingShatter.clear();

	// Burning: advance the char, burn out; snapshot first — BurnOut may remove atoms.
	std::vector<FireState*> burning;
	for (FireState* s : g_states) if (s->burning) burning.push_back(s);
	for (FireState* s : burning)
	{
		Atom* a = s->atom;
		Material* m = a ? FlamMat(a) : nullptr;
		if (!a || !m) { s->burning = false; continue; }
		s->burnT += (float)dt;
		const float dur = std::max(0.1f, m->liveBurn);
		PaintCharFront(a, s, std::min(1.0f, s->burnT / dur), (float)dt);
		if (s->burnT >= dur) BurnOut(w, a, s, m);
	}

	// Heat spread, throttled: burning surfaces cook flammable neighbours into ignition.
	g_spreadIn -= dt;
	if (g_spreadIn <= 0.0)
	{
		const double step = 0.33;
		g_spreadIn = step;
		std::vector<std::pair<Vector3, float>> sources;   // pos + spread radius
		for (FireState* s : g_states)
			if (s->burning && s->atom)
				if (Material* m = FlamMat(s->atom))
					sources.push_back({ s->atom->GetTransform().globalPosition(), m->liveSpread });
		if (!sources.empty())
		{
			const bool budget = BurningCount() < g_maxFires;
			for (long id : g_candidates)
			{
				Atom* a = w->GetById(id);
				if (!a || !a->enabled) continue;
				FireState* s = StateOf(a);
				if (s && (s->burning || s->burned)) continue;
				Material* m = FlamMat(a);
				if (!m) continue;
				Vector3 p = a->GetTransform().globalPosition();
				double gain = 0.0;
				Vector3 hotSrc = p;   // strongest source (the side the fire catches on)
				for (auto& src : sources)
				{
					Vector3 d = p - src.first;
					const double rr = (double)src.second * src.second;
					const double dd = d.x * d.x + d.y * d.y + d.z * d.z;
					if (dd >= rr) continue;
					const double g = 1.0 - std::sqrt(dd / rr);
					if (g > gain) { gain = g; hotSrc = src.first; }
				}
				if (gain <= 0.0)
				{
					if (s && s->heat > 0.0f) s->heat = std::max(0.0f, s->heat - (float)step * 0.5f);
					continue;
				}
				if (!s) s = EnsureState(a);
				s->heat += (float)(step * gain);
				if (s->heat >= m->liveIgnite && budget)
					StartBurn(w, a, s, m, SnapToBounds(a, hotSrc));   // heat-facing face
			}
		}
	}
}

}  // namespace nuke
