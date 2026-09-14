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
	enum Mode : int { None = 0, Procedural = 1, Physical = 2 };   // None = clear color only; Physical = the planet's atmosphere
	[[nuke::prop(label="Mode", enum="None,Procedural,Physical")]] Mode mode = Procedural;

	[[nuke::prop(label="Sky Top")]]     Color skyTop     = Color(0.30, 0.50, 0.90, 1);
	[[nuke::prop(label="Sky Horizon")]] Color skyHorizon = Color(0.70, 0.80, 0.95, 1);
	[[nuke::prop(label="Sky Ground")]]  Color skyGround  = Color(0.20, 0.20, 0.22, 1);
	[[nuke::prop(label="Sky Intensity", min=0, max=4)]] float skyIntensity = 1.0f;

	[[nuke::prop(label="Ambient")]]     Color ambient = Color(0.50, 0.55, 0.60, 1);
	[[nuke::prop(label="Ambient Intensity", min=0, max=2)]] float ambientIntensity = 0.35f;

	// Tonemap (SDR): Exposure scales the HDR scene, White Point is the linear value mapped to pure white.
	[[nuke::prop(label="Exposure", min=0, max=8)]]      float exposure = 1.0f;
	[[nuke::prop(label="White Point", min=0.1, max=8)]] float whitePoint = 1.0f;

	// Physical atmosphere (Mode = Physical): a planet under the camera with a scattering atmosphere;
	// the sky, the aerial perspective, the sun's colour and the view from space all come from it.
	[[nuke::prop(label="Planet Radius (km)", min=1, max=100000)]]     float planetRadius = 6371.0f;
	[[nuke::prop(label="Atmosphere Height (km)", min=1, max=1000)]]   float atmosphereHeight = 100.0f;
	[[nuke::prop(label="Atmosphere Density", min=0, max=10)]]         float atmoDensity = 1.0f;      // scales the whole medium
	[[nuke::prop(label="Rayleigh Color")]]                            Color rayleighColor = Color(0.175, 0.409, 1.0, 1);   // air: blue scatters most
	[[nuke::prop(label="Rayleigh Amount", min=0, max=200)]]           float rayleighAmount = 33.1f;   // scattering at sea level, 1/Mm
	[[nuke::prop(label="Rayleigh Height (km)", min=0.1, max=50)]]     float rayleighHeight = 8.0f;
	[[nuke::prop(label="Mie Amount", min=0, max=200)]]                float mieAmount = 3.996f;       // haze scattering, 1/Mm
	[[nuke::prop(label="Mie Absorption", min=0, max=200)]]            float mieAbsorption = 4.4f;     // 1/Mm
	[[nuke::prop(label="Mie Height (km)", min=0.1, max=20)]]          float mieHeight = 1.2f;
	[[nuke::prop(label="Mie Anisotropy", min=0, max=0.99)]]           float mieAnisotropy = 0.8f;     // forward scatter (the glow around the sun)
	[[nuke::prop(label="Ozone Amount", min=0, max=10)]]               float ozoneAmount = 1.0f;       // 1 = Earth (the blue zenith at dusk)
	[[nuke::prop(label="Ground Albedo")]]                             Color groundAlbedo = Color(0.3, 0.3, 0.3, 1);   // the planet seen from above / the bounce
	[[nuke::prop(label="Aerial Perspective Range (km)", min=1, max=1000)]] float aerialRange = 40.0f;   // the froxel volume's reach
	[[nuke::prop(label="Aerial Perspective", min=0, max=1)]]          float aerialStrength = 1.0f;    // the haze over the geometry

	[[nuke::prop(label="Sun Disk")]]    bool  sunDisk = true;   // draw a sun in the sky from the first directional light
	[[nuke::prop(label="Sun Size (deg)", min=0.05, max=30)]] float sunSize = 1.0f;   // the disc's angular radius; the glow and the sun shafts' source scale with it
	[[nuke::prop(label="Sun Glow", min=0, max=4)]]           float sunGlow = 0.35f;  // the glow around the disc, independent of the light's intensity

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
	[[nuke::prop(label="Moon Light", min=0, max=5)]] float moonLight = 0.15f;  // the moon as a directional light (x visibility x phase): clouds and ground at night
	[[nuke::prop(label="Eclipse", min=0, max=1)]]    float eclipse = 0.0f;     // the moon's transit over the sun: 0 = far left / off, 0.5 = total, 1 = gone right

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
