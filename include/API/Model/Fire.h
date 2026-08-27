#pragma once
#ifndef NUKEE_FIRE_H
#define NUKEE_FIRE_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>
#include <vector>

namespace nuke {

// Per-atom fire state, attached by the fire system (and serialized with the world, so saves
// keep half-burned scenes). Heat accumulates from burning neighbours until the material's
// ignition time; burning drives the material's "burn" condition state 0..1 to burn-out.
class NUKEENGINE_API FireState : public Component
{
	NUKE_CLASS(FireState, Component, "World")
public:
	[[nuke::prop(label="Heat", min=0, tip="Accumulated ignition progress, seconds of exposure")]]
	float heat = 0.0f;
	[[nuke::prop(label="Burning")]] bool burning = false;
	[[nuke::prop(label="Burn Time", min=0, tip="Seconds burned so far")]] float burnT = 0.0f;
	[[nuke::prop(label="Burned", tip="Burned out (charred)")]] bool burned = false;
	[[nuke::prop(hidden)]] Vector3 ignitePos;    // world point the fire started at (front origin)
	[[nuke::prop(hidden)]] Atom* fx = nullptr;   // spawned fire-visual prefab instance
	bool shattered = false;   // debris already spawned (runtime; guards double shatter)

	FireState() : Component("FireState") {}
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
};

// The fire system: flammability lives on LiveMaterial (liveIgnite/liveBurn/liveSpread +
// fire/debris prefabs), the charring visual is the material's own "burn" condition state.
// Burning surfaces heat flammable neighbours; burn-out chars the material and, with a debris
// prefab set, shatters the atom into debris bodies. Broadcasts "fire.ignite" / "fire.out" /
// "fire.destroyed" on the event bus. Heavy and therefore switchable + budgeted.
class NUKEENGINE_API Fire
{
	NUKE_CLASS_NOCREATE(Fire, Object)
public:
	[[nuke::func]] static bool   Ignite(Atom* a);        // false = fireproof/off/over budget
	[[nuke::func]] static bool   IgnitePoint(Atom* a, const Vector3& worldPos);   // fire starts HERE
	[[nuke::func]] static void   IgniteAt(const Vector3& pos, double radius);   // everything flammable in range
	[[nuke::func]] static void   Extinguish(Atom* a);    // stops burning, keeps the char so far
	[[nuke::func]] static bool   Burning(Atom* a);
	[[nuke::func]] static double BurnProgress(Atom* a);  // 0..1 char; -1 = untouched
	[[nuke::func]] static double ActiveFires();
	[[nuke::func]] static void   SetEnabled(bool on);    // global kill switch (fires freeze)
	[[nuke::func]] static bool   Enabled();
	[[nuke::func]] static void   SetMaxFires(double n);  // performance budget (default 64)

	static void Tick(class World* w);                    // World::Update, post-traversal
	// Render-phase spawn pump (fire visuals / debris) — prefab spawns from the game tick race
	// the render pass, so they queue and land here, same as Surface::DrainHits.
	static void Drain(class World* w);
	static void Register(FireState* s);
	static void Unregister(FireState* s);
};

// Destruction MECHANISM only — the game's own damage system decides WHEN. Shatter blows the
// atom into its material's debris prefabs and removes it; "destruct.shatter" hits the bus.
// (Ropes cut via Rope.Cut, joints via JointBase.Break, ragdoll bones via Ragdoll.DetachBone.)
class NUKEENGINE_API Destruct
{
	NUKE_CLASS_NOCREATE(Destruct, Object)
public:
	[[nuke::func]] static bool Shatter(Atom* a);   // false = no debris data on the material
	[[nuke::func]] static bool CanShatter(Atom* a);
};

}  // namespace nuke

#endif // !NUKEE_FIRE_H
