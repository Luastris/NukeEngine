#pragma once
#ifndef NUKEE_REFLECTIONPROBE_H
#define NUKEE_REFLECTIONPROBE_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {

// Captures the surrounding scene into a cubemap from this atom's position; the world shader samples it for
// reflections (specular IBL) so glossy/metal surfaces reflect the actual game world. Reflected Component
// (sibling of a Transform on its atom). World::Render finds the first one, captures it, and binds it.
class NUKEENGINE_API ReflectionProbe : public Component
{
	NUKE_CLASS(ReflectionProbe, Component, "Rendering")
public:
	[[nuke::prop(label="Resolution", enum="64,128,256,512")]] int resolution = 2;   // index -> 64/128/256/512
	[[nuke::prop(label="Near", min=0.01, max=10)]]   float nearZ     = 0.1f;
	[[nuke::prop(label="Far",  min=1,    max=2000)]]  float farZ      = 100.0f;
	[[nuke::prop(label="Intensity", min=0, max=4)]]   float intensity = 1.0f;
	[[nuke::prop(label="Realtime")]]                  bool  realtime  = false;   // re-capture every frame (dynamic)
	[[nuke::prop(label="Bake")]]                      bool  bake      = false;   // tick to force a one-off re-capture
	// Realtime capture budget. 0/6 = all six faces every frame (fully live reflections, the
	// default); 1-5 = that many faces per frame round-robin — up to 6x cheaper, but fast
	// motion in the mirror updates in steps. Opt-in, NOT the default (a sliced mirror turns
	// particles into a slideshow).
	[[nuke::prop(label="Faces Per Frame", min=0, max=6, tip="0 or 6 = capture all faces every frame. 1-5 = time-slice the capture over frames: cheaper, but reflections of fast motion update in steps.")]] int sliceFaces = 0;
	// Parallax correction: anchor the cubemap to a box volume centred on the probe (instead of "reflection at
	// infinity"), so reflections line up with the actual geometry and agree with SSR. Size it to the room.
	[[nuke::prop(label="Box Projection")]]            bool    boxProjection = true;
	[[nuke::prop(label="Box Size", min=0, max=500)]]  Vector3 boxSize       = { 20.0f, 20.0f, 20.0f };

	// Runtime state (not serialized): the renderer cube handle + whether it has been captured.
	uint64_t cubeId   = 0;
	bool     captured = false;
	int      sliceFace = 0;   // round-robin cursor when sliceFaces time-slicing is active
	int      builtRes = 0;

	ReflectionProbe();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	int  Res() const { const int t[] = { 64, 128, 256, 512 }; int i = resolution < 0 ? 0 : (resolution > 3 ? 3 : resolution); return t[i]; }
};
}  // namespace nuke

#endif // !NUKEE_REFLECTIONPROBE_H
