#pragma once
#ifndef NUKEE_ENVIRONMENT_H
#define NUKEE_ENVIRONMENT_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {

// World environment: procedural sky + ambient (and later IBL / fog / time-of-day). One per World;
// World::Render uses the first Environment found. No Environment => just the camera clear color.
class NUKEENGINE_API Environment : public Component
{
	NUKE_CLASS(Environment, Component)
public:
	enum Mode : int { None = 0, Procedural = 1 };   // None = clear color only
	[[nuke::prop(label="Mode", enum="None,Procedural")]] Mode mode = Procedural;

	[[nuke::prop(label="Sky Top")]]     Color skyTop     = Color(0.30, 0.50, 0.90, 1);
	[[nuke::prop(label="Sky Horizon")]] Color skyHorizon = Color(0.70, 0.80, 0.95, 1);
	[[nuke::prop(label="Sky Ground")]]  Color skyGround  = Color(0.20, 0.20, 0.22, 1);
	[[nuke::prop(label="Sky Intensity", min=0, max=4)]] float skyIntensity = 1.0f;

	[[nuke::prop(label="Ambient")]]     Color ambient = Color(0.50, 0.55, 0.60, 1);
	[[nuke::prop(label="Ambient Intensity", min=0, max=2)]] float ambientIntensity = 0.35f;

	// Tonemap (SDR). Exposure scales the HDR scene; White Point is the linear value mapped to pure white — so a
	// fully-lit white surface reads as white (plain Reinhard asymptotes below 1.0 and washes everything to grey).
	[[nuke::prop(label="Exposure", min=0, max=8)]]      float exposure = 1.0f;
	[[nuke::prop(label="White Point", min=0.1, max=8)]] float whitePoint = 1.0f;

	[[nuke::prop(label="Sun Disk")]]    bool  sunDisk = true;   // draw a sun in the sky from the first directional light

	// Time of day (optional): drives the FIRST directional light (rotation/color/intensity) + the sky
	// colours from `hour`. Other lights/suns are untouched. Off => the sky uses the authored colours above.
	[[nuke::prop(label="Time of Day")]]      bool  useTimeOfDay = false;
	[[nuke::prop(label="Hour", min=0, max=24)]]        float hour     = 12.0f;
	[[nuke::prop(label="Day Speed (h/s)", min=0, max=4)]] float daySpeed = 0.0f;   // auto-advance; 0 = manual
	[[nuke::prop(label="Stars")]]            bool  stars = false;   // night-sky stars (fade in when dark)
	[[nuke::prop(asset="texture", label="Stars Texture")]] std::string starsTexGuid;   // equirect panorama; empty = procedural

	[[nuke::prop(label="Moon")]]             bool  moon = false;    // textured moon disk (opposite the sun, visible at night)
	[[nuke::prop(asset="texture", label="Moon Texture")]] std::string moonTexGuid;
	[[nuke::prop(label="Moon Size (deg)", min=0.5, max=30)]] float moonSize = 5.0f;   // angular radius
	[[nuke::prop(label="Moon Phase", min=0, max=1)]] float moonPhase = 0.5f;   // 0/1 = new, 0.5 = full (procedural terminator)

	Environment();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};
}  // namespace nuke

#endif // !NUKEE_ENVIRONMENT_H
