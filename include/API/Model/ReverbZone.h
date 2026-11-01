#pragma once
#ifndef NUKEE_REVERBZONE_H
#define NUKEE_REVERBZONE_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <vector>

namespace nuke {

// A room for an aux reverb: where the listener is inside (or within the blend distance of) this
// volume, the first "reverb" effect of the target bus's chain takes this zone's room, weighted;
// overlapping zones blend in ascending priority, outdoors keeps the mixer's own parameters.
// What the buses send stays the mixer's (their sends); a zone only changes HOW the reverb sounds.
class NUKEENGINE_API ReverbZone : public Component
{
	NUKE_CLASS(ReverbZone, Component, "Audio")
public:
	[[nuke::prop(label="Bus", widget="audiobus", tip="The bus whose reverb effect this zone drives; -1 = every reverb effect in the mix")]] int bus = -1;
	[[nuke::prop(label="Shape", enum="Sphere,Box")]]   int     shape = 1;
	[[nuke::prop(label="Radius", min=0)]]              float   radius = 10.0f;                 // sphere
	[[nuke::prop(label="Half Extents")]]               Vector3 halfExtents = Vector3(5, 5, 5);   // box (local)
	[[nuke::prop(label="Priority", tip="Higher wins where zones overlap (blended in ascending order).")]] int priority = 0;
	[[nuke::prop(label="Blend Distance", min=0, tip="Metres outside the shape over which the room fades back out.")]] float blendDistance = 2.0f;
	[[nuke::prop(label="Weight", min=0, max=1)]]        float   weight = 1.0f;
	[[nuke::prop(label="Room Size", min=0, max=1, tip="Decay length: 0 closet, 1 cathedral")]] float roomSize = 0.8f;
	[[nuke::prop(label="Damping", min=0, max=1, tip="High-frequency loss per reflection")]]    float damping = 0.4f;
	[[nuke::prop(label="Wet", min=0, max=1, tip="The reverb's level while the listener is in this zone")]] float wet = 0.5f;
	[[nuke::prop(label="Width", min=0, max=1)]]         float   width = 1.0f;
	[[nuke::prop(label="Pre-Delay", min=0, max=200, tip="ms before the first reflection (room size cue)")]] float preDelay = 20.0f;

	float WeightAt(const Vector3& p) const;      // 0 outside the blend band .. weight inside
	static const std::vector<ReverbZone*>& All();

	ReverbZone();
	void Init(Atom* parent) override;
	void Update() override {}
	void FixedUpdate() override {}
	void Reset() override {}
	void Pause() override {}
	void Destroy() override;
};

}  // namespace nuke
#endif // !NUKEE_REVERBZONE_H
