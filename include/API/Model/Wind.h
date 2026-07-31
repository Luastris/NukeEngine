#pragma once
#ifndef NUKEE_WIND_H
#define NUKEE_WIND_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <nlohmann/json_fwd.hpp>

namespace nuke {

// Local wind volume: adds to the global wind inside its shape. Directional mode blows along the
// atom's FORWARD, Radial outward from its position. Additive with every other zone.
class NUKEENGINE_API WindZone : public Component
{
	NUKE_CLASS(WindZone, Component, "World")
public:
	[[nuke::prop(label="Shape", enum="Sphere,Box")]]        int   shape = 0;
	[[nuke::prop(label="Radius", min=0)]]                   float radius = 10.0f;      // sphere
	[[nuke::prop(label="Half Extents")]]                    Vector3 halfExtents = Vector3(5, 5, 5);   // box (local)
	[[nuke::prop(label="Mode", enum="Directional,Radial")]] int   mode = 0;
	[[nuke::prop(label="Strength")]]                        float strength = 5.0f;     // m/s (negative radial = suction)
	[[nuke::prop(label="Falloff", min=0, max=1, tip="0 = full strength to the edge, 1 = linear fade to the edge.")]] float falloff = 1.0f;
	// Per-zone animation layered on top of the global wind's own gusts/turbulence.
	[[nuke::prop(label="Turbulence", min=0, tip="m/s of spatial direction noise added inside the zone.")]] float turbulence = 0.0f;
	[[nuke::prop(label="Turbulence Scale", min=0.1, tip="World units per turbulence feature.")]] float turbulenceScale = 4.0f;
	[[nuke::prop(label="Gust Amount", min=0, max=1, tip="0 = steady; 1 = strength swells fully on and off.")]] float gustAmount = 0.0f;
	[[nuke::prop(label="Gust Frequency", min=0.01, tip="Gust cycles per second (approx).")]] float gustFrequency = 0.5f;

	WindZone();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};

// THE wind field: a GLOBAL directional wind (strength + gusts + turbulence, animated over game
// time) plus every enabled WindZone, sampleable at any world point. WORLD STATE: saved in the
// .nuworld "wind" block. The current animated global is pushed to the renderer every frame
// (iRender::setWind -> FrameCB g_Wind/g_Wind2) for shader consumers.
class NUKEENGINE_API Wind
{
	NUKE_CLASS_NOCREATE(Wind, Object)
public:
	// ---- reflected global parameters -------------------------------------------------------
	[[nuke::func]] static void    SetDirection(const Vector3& dir);   // normalized on set (XZ-plane typical, Y allowed)
	[[nuke::func]] static Vector3 Direction();
	[[nuke::func]] static void    SetStrength(double metersPerSec);   // 0 = no global wind (default)
	[[nuke::func]] static double  Strength();
	// Gusts: periodic strength swell. `amount` 0..1 scales the swell (0 = steady wind),
	// `frequency` in Hz-ish (Perlin octave rate over game time).
	[[nuke::func]] static void    SetGusts(double amount, double frequency);
	[[nuke::func]] static double  GustAmount();
	[[nuke::func]] static double  GustFrequency();
	// Turbulence: spatial direction noise. `amount` 0..1 (fraction of strength deflected),
	// `scale` = world-units per noise feature.
	[[nuke::func]] static void    SetTurbulence(double amount, double scale);
	[[nuke::func]] static double  TurbulenceAmount();
	[[nuke::func]] static double  TurbulenceScale();
	// The full wind VECTOR at a world point right now: gusted global + turbulence + zones.
	[[nuke::func]] static Vector3 Sample(const Vector3& worldPos);

	// ---- engine ----------------------------------------------------------------------------
	// Wind's own smooth clock (scaled game delta, once per frame). Must NOT use the calendar's
	// totalgt: whole-second lattice coordinates make gradient Perlin exactly 0, killing gusts.
	static void Advance(double dt);
	// Every enabled WindZone as a shader BendVolume: sphere zones exact, box zones approximated
	// by their bounding sphere. Per-zone gusts are baked into the strength (same clock as Sample).
	static void CollectZones(std::vector<struct BendVolume>& out);
	static void Register(WindZone* z);     // WindZone lifecycle (Init/Destroy)
	static void Unregister(WindZone* z);
	static void SaveJson(nlohmann::json& j);         // world "wind" block
	static void LoadJson(const nlohmann::json& j);
	static void ResetDefaults();                     // world without a wind block
	// The CURRENT animated global for the renderer push: gusted strength along the direction.
	static void ShaderParams(float outDirStrength[4], float outParams[4]);
};

}  // namespace nuke

#endif // !NUKEE_WIND_H
