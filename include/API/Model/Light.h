#pragma once
#ifndef NUKEE_LIGHT_H
#define NUKEE_LIGHT_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"
#include "render/irender.h"   // NukeLight (FrameLights submissions)
#include <vector>

namespace nuke {

// A scene light. World::Render gathers every enabled Light each frame and pushes them to the renderer
// (iRender::setLights) for the PBR pass. Position = the atom's world position; direction = its forward.
class NUKEENGINE_API Light : public Component
{
	NUKE_CLASS(Light, Component, "Rendering")
public:
	enum Type : int { Directional = 0, Point = 1, Spot = 2 };
	[[nuke::prop(label="Type", enum="Directional,Point,Spot")]] Type type = Directional;
	[[nuke::prop(label="Color")]]      Color color = Color(1, 1, 1, 1);
	[[nuke::prop(label="Intensity")]]  float intensity = 3.0f;
	[[nuke::prop(label="Range")]]      float range     = 10.0f;          // point/spot falloff distance
	[[nuke::prop(label="Spot Angle")]] float spotAngle = 35.0f;          // spot outer cone half-angle (deg)
	[[nuke::prop(label="Spot Blend")]] float spotBlend = 0.15f;          // 0..1 inner/outer cone softness
	[[nuke::prop(label="Cast Shadows")]] bool castShadows = true;        // this light projects shadows

	Light();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};

// DYNAMIC per-frame lights from modules/effects (glowing particles, muzzle flashes): submit
// during Update/OnRender; World::Render appends them to the gathered scene lights and CLEARS
// the list. A submission lives ONE frame — resubmit every frame while the source shines.
// Game thread only. The renderer's light budget (16) applies to the combined set.
class NUKEENGINE_API FrameLights
{
public:
	static void Submit(const NukeLight& l);
	static std::vector<NukeLight>& Frame();   // World::Render: append into setLights, then clear
};
}  // namespace nuke

#endif // !NUKEE_LIGHT_H
