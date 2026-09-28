#pragma once
#ifndef NUKEE_DESTRUCTIBLE_H
#define NUKEE_DESTRUCTIBLE_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nuke {

class World;
class iPhysics;

// P2 destruction. The atom's mesh is a Voronoi set of pieces (baked into the .numesh or cut
// once at runtime). A hard enough impact breaks it: every piece stands up as its own atom -
// the ones near the hit fly off as debris bodies, the rest stay a STRUCTURE (static bodies
// joined by a connectivity graph; a group left without support collapses). Debris lives by
// lifetime / sleep / count budgets. "destruct.break" reaches the event bus; the material's
// debris prefabs (dust, splinters) spawn as in the fire system.
class NUKEENGINE_API Destructible : public Component
{
	NUKE_CLASS(Destructible, Component, "Physics")
public:
	[[nuke::prop(label="Pieces", min=2, tip="Voronoi pieces the mesh breaks into (bake them into the mesh below; otherwise cut once at first use).")]] int pieces = 12;
	[[nuke::prop(label="Seed", min=0)]] int seed = 1;
	[[nuke::prop(label="Break Speed", min=0, tip="Impact speed (m/s, relative, along the contact normal) that breaks it. 0 = any touch.")]] float breakSpeed = 4.0f;
	[[nuke::prop(label="Impact Radius", min=0, tip="Pieces whose centre lies within this distance of the hit fly off (the nearest one always does). Metres.")]] float impactRadius = 0.6f;
	[[nuke::prop(label="Structural", tip="The remaining pieces stay a structure: static bodies joined to their neighbours; a group that loses every support (touching ground / another static body) collapses. Off = everything flies at once.")]] bool structural = true;
	[[nuke::prop(label="Kick", min=0, tip="Impulse given to the pieces that fly off, x their mass, along the hit and outward.")]] float kick = 1.0f;
	[[nuke::prop(label="Debris Lifetime", min=0, tip="Seconds a flying piece lives (0 = forever).")]] float debrisLifetime = 30.0f;
	[[nuke::prop(label="Debris Sleep", min=0, tip="Seconds a flying piece may lie still before it is removed (0 = never).")]] float debrisSleep = 8.0f;
	[[nuke::prop(label="Debris Max", min=0, tip="Flying pieces this atom keeps alive at once; the oldest goes first (0 = all).")]] int debrisMax = 64;

	// Runtime (saved: a broken structure loads broken).
	[[nuke::prop(hidden)]] bool broken = false;
	[[nuke::prop(hidden)]] std::vector<int> heldPieces;   // piece index -> atom id of the STRUCTURE piece (0 = gone)

	Destructible();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override;
	TimeDomain timeDomain() const override { return TimeDomain::Physics; }

	// ---- script surface ----
	[[nuke::func]] bool Break(const Vector3& worldPoint, double radius);   // break here (any speed); false = can't fracture
	[[nuke::func]] bool Shatter();                                          // every piece flies
	[[nuke::func]] double PieceCount();                                     // pieces of the cut (0 = can't fracture)
	[[nuke::func]] double HeldCount();                                      // pieces still standing in the structure
	[[nuke::func]] static double DebrisCount();                             // flying pieces alive, every destructible
	[[nuke::func]] static void   SetMaxDebris(double n);                    // global cap on flying pieces (0 = none)

	// ---- engine plumbing ----
	// A contact from the fixed step (World::DispatchContacts): `self` is the destructible atom or one
	// of its structure pieces. Queues the break; nothing spawns inside the physics dispatch.
	static void Contact(Atom* self, Atom* other, float speed, const float point[3], const float normal[3]);
	static void Drain(World* w);          // spawns / detaches queued breaks (a safe point of the frame)
	static void Tick(World* w, float dt); // debris budgets
	static Destructible* Of(Atom* a);      // the destructible that owns `a` (itself or a structure piece)

private:
	struct PendingBreak { long owner; Vector3 point; Vector3 normal; double radius; bool all; };
	static std::vector<PendingBreak> gPending;
	void DoBreak(World* w, const Vector3& point, const Vector3& normal, double radius, bool all);
	bool SpawnPieces(World* w);
	void Detach(World* w, int index, const Vector3& point, const Vector3& normal, bool kickIt);
	void Collapse(World* w, iPhysics* p);
	std::vector<std::vector<int>> adjacency;   // piece -> neighbouring pieces (runtime)
	Vector3 srcPos; Quaternion srcRot; Vector3 srcScale;   // the pose the structure was broken at
};

// The marker a piece atom carries: which destructible / piece it is (saved with the atom).
class NUKEENGINE_API DebrisPiece : public Component
{
	NUKE_CLASS(DebrisPiece, Component, "Physics")
public:
	[[nuke::prop(hidden)]] int owner = 0;        // atom id of the Destructible
	[[nuke::prop(hidden)]] int piece = -1;
	[[nuke::prop(hidden)]] bool flying = false;  // debris (true) or a held structure piece (false)
	float age = 0.0f, still = 0.0f;              // runtime: seconds alive, seconds lying still

	DebrisPiece();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
	TimeDomain timeDomain() const override { return TimeDomain::Physics; }
};

}  // namespace nuke

#endif // !NUKEE_DESTRUCTIBLE_H
