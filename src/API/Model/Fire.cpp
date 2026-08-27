// Fire system: LiveMaterial-driven ignition/spread/burn-out + debris destruction (see Fire.h).
#include "API/Model/Fire.h"
#include "API/Model/Atom.h"
#include "API/Model/Events.h"
#include "API/Model/Game.h"
#include "API/Model/Material.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Prefab.h"
#include "API/Model/Rigidbody.h"
#include "API/Model/Surface.h"
#include "API/Model/Time.h"
#include "API/Model/World.h"
#include "interface/AppInstance.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace nuke {

namespace {

bool   g_enabled = true;
int    g_maxFires = 64;
double g_lastTick = -1.0;
double g_scanIn = 0.0;      // seconds until the flammable-candidate rescan
double g_spreadIn = 0.0;    // seconds until the next heat-spread pass
std::vector<FireState*> g_states;
std::vector<long> g_candidates;      // atom ids with a flammable material
std::vector<long> g_pendingIgnite;   // queued from scripts, applied post-traversal

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

// Start burning: state flip + the material's looping fire visual (spawned by Drain).
void StartBurn(World* w, Atom* a, FireState* s, Material* m)
{
	(void)w;
	s->burning = true;
	s->burnT = std::max(0.0f, s->burnT);
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

// Burn-out: full char stays; a debris prefab shatters the atom into bodies and removes it.
void BurnOut(World* w, Atom* a, FireState* s, Material* m)
{
	s->burning = false;
	s->burned = true;
	StopFx(w, s);
	EmitFire("fire.out", a);
	if (!m->liveDebrisPrefab.empty())
	{
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
		EmitFire("fire.destroyed", a);
		w->QueueDestroy(a->id.id);
	}
}

// The material's char state follows burn progress (the "burn" condition on this atom).
void DriveChar(Atom* a, float value)
{
	SurfaceState* ss = Surface::StateOn(a);
	if (!ss)
	{
		ss = new SurfaceState();
		ss->Init(a);
	}
	ss->SetState("burn", std::min(1.0f, value));
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
	if (!w || g_pendingSpawns.empty()) return;
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
			fx->GetTransform().position = Vector3(0, 0, 0);   // ride the burning atom
			s->fx = fx;
		}
	}
}

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
	g_pendingIgnite.push_back(a->id.id);
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
		Vector3 d = a->GetTransform().globalPosition() - p;
		if (d.x * d.x + d.y * d.y + d.z * d.z <= radius * radius) Ignite(a);
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
	for (long id : g_pendingIgnite)
	{
		Atom* a = w->GetById(id);
		Material* m = a ? FlamMat(a) : nullptr;
		if (!a || !m) continue;
		FireState* s = EnsureState(a);
		if (!s->burning && !s->burned) StartBurn(w, a, s, m);
	}
	g_pendingIgnite.clear();

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
		DriveChar(a, s->burnT / dur);
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
				for (auto& src : sources)
				{
					Vector3 d = p - src.first;
					const double rr = (double)src.second * src.second;
					const double dd = d.x * d.x + d.y * d.y + d.z * d.z;
					if (dd < rr) gain = std::max(gain, 1.0 - std::sqrt(dd / rr));   // nearest source wins
				}
				if (gain <= 0.0)
				{
					if (s && s->heat > 0.0f) s->heat = std::max(0.0f, s->heat - (float)step * 0.5f);
					continue;
				}
				if (!s) s = EnsureState(a);
				s->heat += (float)(step * gain);
				if (s->heat >= m->liveIgnite && budget) StartBurn(w, a, s, m);
			}
		}
	}
}

}  // namespace nuke
