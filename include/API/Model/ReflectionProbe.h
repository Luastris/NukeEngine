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
	NUKE_CLASS(ReflectionProbe, Component)
public:
	[[nuke::prop(label="Resolution", enum="64,128,256,512")]] int resolution = 2;   // index -> 64/128/256/512
	[[nuke::prop(label="Near", min=0.01, max=10)]]   float nearZ     = 0.1f;
	[[nuke::prop(label="Far",  min=1,    max=2000)]]  float farZ      = 100.0f;
	[[nuke::prop(label="Intensity", min=0, max=4)]]   float intensity = 1.0f;
	[[nuke::prop(label="Realtime")]]                  bool  realtime  = false;   // re-capture every frame (dynamic)
	[[nuke::prop(label="Bake")]]                      bool  bake      = false;   // tick to force a one-off re-capture
	// Parallax correction: anchor the cubemap to a box volume centred on the probe (instead of "reflection at
	// infinity"), so reflections line up with the actual geometry and agree with SSR. Size it to the room.
	[[nuke::prop(label="Box Projection")]]            bool    boxProjection = true;
	[[nuke::prop(label="Box Size", min=0, max=500)]]  Vector3 boxSize       = { 20.0f, 20.0f, 20.0f };

	// Runtime state (not serialized): the renderer cube handle + whether it has been captured.
	uint64_t cubeId   = 0;
	bool     captured = false;
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
