#pragma once
#ifndef NUKEE_QUALITY_H
#define NUKEE_QUALITY_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

class iRender;

// Scalability presets over the EXISTING toggles. The world's authored settings are the ceiling:
// a preset caps them per feature (tier 0 Low .. 3 Ultra) at the moment they reach the renderer,
// so a world load never wipes the choice and the editor's authored values stay untouched. Every
// feature can be overridden on its own (the preset then reads "Custom"). Persisted in
// config/main.json ["quality"] (the game writes it; the editor keeps it for the session); a first
// boot without one autodetects a preset from the GPU (VRAM, ray tracing, discrete) and the CPU.
// Features: raytracing (boot flag, next launch), reflections (RT reflection stage + bounces),
// shadows (map resolution + distance), ao, gi (DDGI on/off), ssgi, volumetrics, clouds, water
// (NukeWater FX flags), foliage (density), particles (counts), tessellation, postfx (motion blur /
// depth of field), textures (streaming budget), upscale (upscaler quality).
// Cvars: q.preset + q.<feature> (console + the editor's Cvars panel). Event: "quality.changed".
class NUKEENGINE_API Quality
{
	NUKE_CLASS_NOCREATE(Quality, Object)
public:
	[[nuke::func]] static bool        SetPreset(const std::string& name);   // Low / Medium / High / Ultra: every feature, overrides cleared
	[[nuke::func]] static std::string Preset();                             // the preset, "Custom" once a feature is overridden
	[[nuke::func]] static bool        Set(const std::string& feature, int tier);   // override one feature (0..3)
	[[nuke::func]] static int         Get(const std::string& feature);
	[[nuke::func]] static bool        Reset(const std::string& feature);    // back to the preset's tier
	[[nuke::func]] static std::string Features();                           // comma-separated
	[[nuke::func]] static std::string Describe(const std::string& feature); // "shadows = 2 (High): ..."
	[[nuke::func]] static std::string Autodetect();                         // pick + apply a preset from the hardware; returns it
	[[nuke::func]] static std::string AdapterName();
	[[nuke::func]] static double      AdapterMemoryMB();
	[[nuke::func]] static bool        AdapterRayTracing();

	// ---- native ----
	static void  Boot(iRender* r);            // after render init (both hosts): first-boot autodetect + the live knobs
	static int   Version();                   // bumps on every change (consumers re-apply)
	static int   Tier(const char* feature);   // 0..3
	static const char* TierName(int tier);
	// Caps, applied where the authored values reach the renderer.
	static int   CapShadowRes(int authored);
	static float ShadowDistanceScale();
	static int   CapAO(int authored);
	static int   CapSSGI(int authored);
	static bool  GIEnabled(bool authored);
	static int   CapVolumetrics(int authored);
	static int   CapClouds(int authored);
	static bool  KeepPostStage(const char* shaderName);   // rtreflect / motionblur / dof by tier
	static int   CapRTBounces(int authored);
	static float FoliageScale();
	static float ParticleScale();
	static float TessellationScale();
	static bool  RayTracingWanted();          // the boot flag the preset asks for (window.rayTracing)
	static void  BindCvars();                 // q.preset + q.<feature>
};

}  // namespace nuke

#endif // !NUKEE_QUALITY_H
