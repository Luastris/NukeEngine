#pragma once
#ifndef NUKEE_ENVIRONMENT_H
#define NUKEE_ENVIRONMENT_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {

// World environment: procedural sky + ambient. World::Render uses the first Environment found;
// none => just the camera clear color.
class NUKEENGINE_API Environment : public Component
{
	NUKE_CLASS(Environment, Component, "World")
public:
	enum Mode : int { None = 0, Procedural = 1 };   // None = clear color only
	[[nuke::prop(label="Mode", enum="None,Procedural")]] Mode mode = Procedural;

	[[nuke::prop(label="Sky Top")]]     Color skyTop     = Color(0.30, 0.50, 0.90, 1);
	[[nuke::prop(label="Sky Horizon")]] Color skyHorizon = Color(0.70, 0.80, 0.95, 1);
	[[nuke::prop(label="Sky Ground")]]  Color skyGround  = Color(0.20, 0.20, 0.22, 1);
	[[nuke::prop(label="Sky Intensity", min=0, max=4)]] float skyIntensity = 1.0f;

	[[nuke::prop(label="Ambient")]]     Color ambient = Color(0.50, 0.55, 0.60, 1);
	[[nuke::prop(label="Ambient Intensity", min=0, max=2)]] float ambientIntensity = 0.35f;

	// Tonemap (SDR): Exposure scales the HDR scene, White Point is the linear value mapped to pure white.
	[[nuke::prop(label="Exposure", min=0, max=8)]]      float exposure = 1.0f;
	[[nuke::prop(label="White Point", min=0.1, max=8)]] float whitePoint = 1.0f;

	[[nuke::prop(label="Sun Disk")]]    bool  sunDisk = true;   // draw a sun in the sky from the first directional light

	// Time of day: drives the FIRST directional light + sky colours from `hour`; other lights untouched.
	[[nuke::prop(label="Time of Day")]]      bool  useTimeOfDay = false;
	[[nuke::prop(label="Hour", min=0, max=24)]]        float hour     = 12.0f;
	[[nuke::prop(label="Day Speed (h/s)", min=0, max=4)]] float daySpeed = 0.0f;   // auto-advance; 0 = manual
	[[nuke::prop(label="Stars")]]            bool  stars = false;   // night-sky stars (fade in when dark)
	[[nuke::prop(asset="texture", label="Stars Texture")]] std::string starsTexGuid;   // equirect panorama; empty = procedural

	[[nuke::prop(label="Moon")]]             bool  moon = false;    // textured moon disk (opposite the sun, visible at night)
	[[nuke::prop(asset="texture", label="Moon Texture")]] std::string moonTexGuid;
	[[nuke::prop(label="Moon Size (deg)", min=0.5, max=30)]] float moonSize = 5.0f;   // angular radius
	[[nuke::prop(label="Moon Phase", min=0, max=1)]] float moonPhase = 0.5f;   // 0/1 = new, 0.5 = full (procedural terminator)

	// Volumetric clouds: a layer around the planet above sea level (y 0), lit by the sun and the sky.
	enum CloudQuality : int { CloudLow = 0, CloudMedium = 1, CloudHigh = 2 };
	[[nuke::prop(label="Clouds")]]                                    bool  clouds = false;
	[[nuke::prop(label="Cloud Coverage", min=0, max=1)]]              float cloudCoverage = 0.5f;
	[[nuke::prop(label="Cloud Type", min=0, max=1)]]                  float cloudType = 0.5f;        // 0 = stratus, 1 = cumulus
	[[nuke::prop(label="Cloud Density", min=0, max=4)]]               float cloudDensity = 1.0f;
	[[nuke::prop(label="Cloud Bottom (m)", min=0, max=20000)]]        float cloudBottom = 1500.0f;
	[[nuke::prop(label="Cloud Thickness (m)", min=10, max=20000)]]    float cloudThickness = 3500.0f;
	[[nuke::prop(label="Cloud Shape Scale (m)", min=100, max=100000)]]  float cloudShapeScale = 8000.0f;
	[[nuke::prop(label="Cloud Detail Scale (m)", min=10, max=10000)]]   float cloudDetailScale = 900.0f;
	[[nuke::prop(label="Cloud Erosion", min=0, max=1)]]               float cloudErosion = 0.35f;
	[[nuke::prop(label="Weather Scale (m)", min=1000, max=500000)]]   float cloudWeatherScale = 40000.0f;
	[[nuke::prop(label="Cloud Wind Influence", min=0, max=10)]]       float cloudWindInfluence = 1.0f;   // global wind x this
	[[nuke::prop(label="Cloud Drift Speed (m/s)", min=0, max=200)]]   float cloudDriftSpeed = 5.0f;
	[[nuke::prop(label="Cloud Drift Direction (deg)", min=0, max=360)]] float cloudDriftDirection = 0.0f;
	[[nuke::prop(label="Cloud Sun Light", min=0, max=4)]]             float cloudSunIntensity = 1.0f;
	[[nuke::prop(label="Cloud Sky Light", min=0, max=4)]]             float cloudAmbientIntensity = 1.0f;
	[[nuke::prop(label="Cloud Forward Scatter", min=0, max=0.99)]]    float cloudForwardScatter = 0.8f;
	[[nuke::prop(label="Cloud Back Scatter", min=-0.99, max=0)]]      float cloudBackScatter = -0.3f;
	[[nuke::prop(label="Cloud Multi-Scatter", min=0, max=1)]]         float cloudMultiScatter = 0.5f;
	[[nuke::prop(label="Cloud Multi-Scatter Falloff", min=0.05, max=1)]] float cloudMultiScatterFalloff = 0.5f;
	[[nuke::prop(label="Cloud Silver Lining", min=0, max=1)]]         float cloudSilverLining = 0.5f;
	[[nuke::prop(label="Cloud Shadows")]]                             bool  cloudShadows = true;
	[[nuke::prop(label="Cloud Shadow Strength", min=0, max=1)]]       float cloudShadowStrength = 0.8f;
	[[nuke::prop(label="Cloud Shadow Area (m)", min=100, max=50000)]] float cloudShadowArea = 4000.0f;
	[[nuke::prop(label="Cloud Quality", enum="Low,Medium,High")]]     CloudQuality cloudQuality = CloudMedium;
	[[nuke::prop(label="Cloud Max Distance (m)", min=1000, max=500000)]] float cloudMaxDistance = 60000.0f;

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
