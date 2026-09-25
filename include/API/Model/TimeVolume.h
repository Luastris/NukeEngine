#pragma once
#ifndef NUKEE_TIMEVOLUME_H
#define NUKEE_TIMEVOLUME_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "API/Model/Component.h"
#include "reflect/Reflect.h"
#include <string>
#include <vector>

namespace nuke {

// Time volume: the flow of time INSIDE a shape. Every atom inside ticks with dt x Time Scale
// (0 = frozen, <1 slow motion, >1 fast forward) — scripts, movers, animators, particles, the
// rigid bodies (per-body scaling in the physics provider) and the pitch of its sound sources,
// each domain by its own switch. Outside the shape the world runs as before; the influence
// fades over the blend distance; overlapping volumes stack by priority like post volumes. The
// global Game.TimeScale stays what it is: a volume is a local multiplier on top of it. Cameras
// and the tagged player atom can be exempt ("the world froze, I still move").
class NUKEENGINE_API TimeVolume : public Component
{
	NUKE_CLASS(TimeVolume, Component, "World")
public:
	[[nuke::prop(label="Shape", enum="Sphere,Box")]]   int     shape = 1;
	[[nuke::prop(label="Radius", min=0)]]              float   radius = 10.0f;                 // sphere
	[[nuke::prop(label="Half Extents")]]               Vector3 halfExtents = Vector3(5, 5, 5);   // box (local)
	[[nuke::prop(label="Priority", tip="Higher wins where volumes overlap (blended in ascending order).")]] int priority = 0;
	[[nuke::prop(label="Blend Distance", min=0, tip="Metres outside the shape over which the influence fades to nothing.")]] float blendDistance = 2.0f;
	[[nuke::prop(label="Weight", min=0, max=1, tip="Full influence inside the shape (1 = time inside runs exactly at Time Scale).")]] float weight = 1.0f;
	[[nuke::prop(label="Time Scale", min=0, tip="0 = frozen, below 1 slow motion, above 1 fast forward; multiplies the global Game.TimeScale.")]] float timeScale = 0.25f;
	// What the volume touches (one switch per time domain, see Component::timeDomain).
	[[nuke::prop(label="Logic", tip="Scripts, movers, splines - every component not in the domains below")]] bool affectLogic = true;
	[[nuke::prop(label="Animation", tip="Animators, spring bones, paired animation")]]        bool affectAnimation = true;
	[[nuke::prop(label="Physics", tip="Rigid bodies inside: velocities and gravity scaled per body")]] bool affectPhysics = true;
	[[nuke::prop(label="Particles", tip="Particle emitters inside")]]                          bool affectParticles = true;
	[[nuke::prop(label="Audio", tip="Pitch of the sound sources inside")]]                     bool affectAudio = true;
	[[nuke::prop(label="Exempt Cameras", tip="Atoms carrying a Camera keep normal time")]]     bool exemptCameras = true;
	[[nuke::prop(label="Exempt Tag", tip="Atoms with this tag, and everything under them, keep normal time (the player in a frozen world)")]] std::string exemptTag = "Player";

	TimeVolume();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	// Influence 0..1 of this volume at a world point: 1 inside, fading over the blend distance.
	float WeightAt(const Vector3& p) const;
	// Every live volume (registered by Init, dropped by Destroy).
	static const std::vector<TimeVolume*>& All();
	static bool Any();
	// The time multipliers an atom gets this frame, one per TimeDomain (1 = untouched): every
	// live volume blended by priority, its switches and exemptions applied.
	static void ScalesFor(Atom* a, float out[5]);
};

}  // namespace nuke

#endif // !NUKEE_TIMEVOLUME_H
