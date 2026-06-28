#pragma once
#ifndef NUKEE_LIGHT_H
#define NUKEE_LIGHT_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {

// A scene light. World::Render gathers every enabled Light each frame and pushes them to the renderer
// (iRender::setLights) for the PBR pass. Position = the atom's world position; direction = its forward.
class NUKEENGINE_API Light : public Component
{
	NUKE_CLASS(Light, Component)
public:
	[[nuke::prop(label="Type (0=Dir 1=Point 2=Spot)")]] int type = 0;   // 0 directional, 1 point, 2 spot
	[[nuke::prop(label="Color")]]      Color color = Color(1, 1, 1, 1);
	[[nuke::prop(label="Intensity")]]  float intensity = 3.0f;
	[[nuke::prop(label="Range")]]      float range     = 10.0f;          // point/spot falloff distance
	[[nuke::prop(label="Spot Angle")]] float spotAngle = 35.0f;          // spot outer cone half-angle (deg)
	[[nuke::prop(label="Spot Blend")]] float spotBlend = 0.15f;          // 0..1 inner/outer cone softness

	Light();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};
}  // namespace nuke

#endif // !NUKEE_LIGHT_H
