#pragma once
#ifndef NUKEE_FORCEFIELD_H
#define NUKEE_FORCEFIELD_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <vector>

namespace nuke {

// A frame snapshot of one field, for simulations that must not touch the registry from jobs.
struct ForceFieldSnap { int mode; float center[3]; float radius; float strength; float falloff; float axis[3]; float pull; };   // axis = the vortex axis; pull = in (+) / out (-)

// Local force volume: attract / repel / vortex / turbulence inside a sphere, additive, on any
// atom. A general mechanic: particles, fluid fog and foliage bend all read it (the field submits
// itself as a BendVolume each frame, which is how the renderer-side systems see it).
class NUKEENGINE_API ForceField : public Component
{
	NUKE_CLASS(ForceField, Component, "Effects")
public:
	[[nuke::prop(label="Mode", enum="Attract,Repel,Vortex,Turbulence")]] int mode = 1;
	[[nuke::prop(label="Vortex Axis", enum="Free,Atom Up", tip="Free: the vortex turns around the world's up. Atom Up: around the atom's up axis - rotate the atom to pick the plane.")]] int vortexAxis = 0;
	[[nuke::prop(label="Vortex Pull", min=-1, max=1, tip="0: turns in place. 0..1: pulls into the centre (the inner turns faster, angular momentum). -1..0: pushes out of it.")]] float vortexPull = 0.0f;
	[[nuke::prop(label="Inner Radius", min=0, tip="The funnel's core: inside it the turn is solid-body (0 = 15% of the radius). Radius is the outer edge.")]] float vortexInner = 0.0f;
	[[nuke::prop(label="Dent Depth", min=0, max=1, tip="How deep the funnel dents the fog: 0 = the fog's own noise only, 1 = the folds go to clear air.")]] float dentDepth = 0.7f;
	[[nuke::prop(label="Dent Sharpness", min=0, max=1, tip="0 = soft rolls, 1 = crisp ridges.")]] float dentSharp = 0.5f;
	[[nuke::prop(label="Dent Size", min=0, tip="Size of a dent, metres (0 = the fog volume's Noise Scale).")]] float dentSize = 0.0f;
	[[nuke::prop(label="Dent Density", min=0, tip="Extra density in the folds (1 = the folds are twice as dense as the fog around) - the funnel shows from the side too.")]] float dentDensity = 0.5f;
	[[nuke::prop(label="Radius", min=0)]]    float radius = 5.0f;
	[[nuke::prop(label="Strength")]]         float strength = 10.0f;   // m/s^2 at the center
	[[nuke::prop(label="Falloff", min=0, max=1, tip="0 = full strength to the edge, 1 = linear fade.")]] float falloff = 1.0f;

	ForceField();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
	void OnRender(iRender* r, RenderPhase phase) override;   // selected-field gizmo + bend-volume submit

	// Every enabled field, once per frame (thread-safe against Init/Destroy).
	static void Snapshot(std::vector<ForceFieldSnap>& out);

	unsigned long long bendSubmitFrame = ~0ull;   // once-per-frame guard (OnRender runs per pass)
};

}  // namespace nuke

#endif
