#pragma once
#ifndef NUKEE_SURFACE_H
#define NUKEE_SURFACE_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <nlohmann/json_fwd.hpp>
#include <map>
#include <string>
#include <vector>

namespace nuke {

// LiveMaterial condition driver. A CONDITION is a named 0..1 value ("wet", "snow", "dust",
// "rust", ...) that live materials respond to. Values resolve in three layers:
//   1. the WORLD environment (Surface facade below — rain wets everything),
//   2. per-atom overrides (SurfaceState component — this house stays dry),
//   3. painted masks (SurfaceMask component — this exact patch is rusty),
// effective = max(override-or-global, mask at the point).

// Per-atom condition overrides: replaces the world's value for the listed states on this atom
// (and, when resolving, its children — the nearest ancestor override wins).
class NUKEENGINE_API SurfaceState : public Component
{
	NUKE_CLASS(SurfaceState, Component, "World")
public:
	[[nuke::prop(label="States", tip="Condition ids overridden here (wet/snow/dust/rust/...)")]]
	std::vector<std::string> states;
	[[nuke::prop(label="Values", tip="Value 0..1 for each state above, by index")]]
	std::vector<float> values;

	[[nuke::func]] void   SetState(const std::string& state, double value);   // add or update
	[[nuke::func]] void   ClearState(const std::string& state);
	// This override's value for `state`; false = not overridden here.
	bool Value(const std::string& state, float& out) const;

	SurfaceState() : Component("SurfaceState") {}
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
};

// Painted condition mask: a small RGBA grid over a local-space box, each channel bound to a
// state id. Painted with a world-space sphere brush (editor tool or scripts); sampled by
// world position. Serialized as a base64 blob in the hidden `data` prop.
class NUKEENGINE_API SurfaceMask : public Component
{
	NUKE_CLASS(SurfaceMask, Component, "World")
public:
	[[nuke::prop(label="State R", tip="Condition id painted into the R channel")]] std::string state0 = "wet";
	[[nuke::prop(label="State G")]] std::string state1;
	[[nuke::prop(label="State B")]] std::string state2;
	[[nuke::prop(label="State A")]] std::string state3;
	[[nuke::prop(label="Resolution", min=4, max=64, tip="Grid cells per axis (memory = res^3 x 4 bytes)")]]
	int resolution = 16;
	[[nuke::prop(label="Half Extents", tip="Local-space box the grid spans around the atom")]]
	Vector3 halfExtents = Vector3(2, 2, 2);
	[[nuke::prop(hidden)]] std::string data;   // base64 RGBA8 grid — the serialized store

	// Stamp a sphere: channel 0..3, amount added per call (negative erases), soft falloff.
	[[nuke::func]] void   Paint(const Vector3& worldPos, double radius, double channel, double amount);
	[[nuke::func]] void   Clear();
	[[nuke::func]] double SampleChannel(const Vector3& worldPos, double channel);   // 0..1 (0 outside)
	// Mask value for a state id at a world point; 0 when the state has no channel here.
	float Sample(const Vector3& worldPos, const std::string& state);
	int   StateSlot(const std::string& state) const;   // -1 = state not painted by this mask

	SurfaceMask() : Component("SurfaceMask") {}
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
	void OnBeforeSave() override;   // pack `grid` back into the `data` blob

	// runtime (not serialized)
	std::vector<unsigned char> grid;   // res^3 * 4, XYZ-major
	bool decoded = false;
	int  version = 0;                  // bumped on every edit (invalidates the GPU flipbook)
	void EnsureDecoded();
	// GPU copy of the grid, encoded as a 2D flipbook (width = res*res, height = res; Z slabs
	// side by side). Rebuilt lazily when `version` moved; the renderer samples it in world.ps.
	class Texture* GpuTex();
	class Texture* gpuTex = nullptr;
	int  gpuVersion = -1;

private:
	bool WorldToCell(const Vector3& worldPos, float& cx, float& cy, float& cz) const;
};

// The world's global conditions + the resolve entry point. WORLD STATE: saved in the
// .nuworld "surface" block; ResetDefaults on worlds without one.
class NUKEENGINE_API Surface
{
	NUKE_CLASS_NOCREATE(Surface, Object)
public:
	[[nuke::func]] static void   SetCondition(const std::string& state, double value);   // 0 removes
	[[nuke::func]] static double Condition(const std::string& state);
	[[nuke::func]] static void   ClearConditions();
	// Effective value for an atom at a world point: nearest-ancestor SurfaceState override
	// (else the global), maxed with every SurfaceMask along the ancestor chain.
	[[nuke::func]] static double ValueAt(Atom* atom, const std::string& state, const Vector3& worldPos);
	// A state can matter this frame: set globally, overridden on some atom or painted by some
	// mask. Dormant states take no GPU overlay slot (rebuilt once per frame by DriveFoliage).
	static bool StateInUse(const std::string& state);
	static void RefreshInUse();

	// ---- engine ----------------------------------------------------------------------------
	static const std::map<std::string, float>& All();
	// LiveMaterial auto-foliage: grow/refresh a TRANSIENT Foliage per liveFoliage entry on
	// every atom whose MeshRenderer material carries them (called from World::Render).
	static void DriveFoliage(class World* w);
	// Per-draw overlay context: writes the atom's effective uniform values (nearest-ancestor
	// override, else global), the nearest painted mask (as a GPU flipbook) and the world->mask
	// transform into the material's liveDraw* fields right before its draw is submitted.
	static void PushDrawContext(Atom* atom, class Material* m);

	// ---- LM-4 surface responses -------------------------------------------------------------
	// Footstep for `self` (a character): raycasts down from its position (ignoring its own
	// body), picks the ground material's round-robin step clip and plays it 3D with a slight
	// pitch variation. Returns false when nothing is underfoot or the surface has no steps.
	[[nuke::func]] static bool Footstep(Atom* self, double volume);
	// Typed hit on an atom's surface ("bullet"/"blunt"/...; empty matches the material's
	// any-hit entry): plays the reaction sound now and queues the prefab/decal spawn for the
	// next frame (outside the physics step). `impulse` gates entries by their Min Impulse.
	[[nuke::func]] static bool Hit(Atom* atom, const std::string& hitType, const Vector3& pos,
	                               const Vector3& normal, double impulse);
	// The surface identity of an atom's material for gameplay queries ("" = untagged).
	[[nuke::func]] static std::string TagAt(Atom* atom);
	// Footstep with the ground already known (CharacterController::GroundAtom and friends).
	static bool FootstepOn(Atom* ground, const Vector3& pos, double volume);
	// engine: automatic impact reactions from solid contact-begin events (World physics
	// dispatch); the closing speed along the normal gates Min Impulse (m/s).
	static void ContactHit(Atom* a, Atom* b, const float point[3], const float normal[3]);
	// Hit with an EXPLICIT target world for the spawns (editor previews); plain Hit() targets
	// the live pump's world.
	static bool HitIn(World* target, Atom* atom, const std::string& hitType, const Vector3& pos,
	                  const Vector3& normal, double impulse);
	// Drain the queued hit spawns (prefab/decal) into `w` and expire their lifetimes.
	// DriveFoliage pumps this for the live world; the editor pumps it for preview worlds
	// (called right after a preview Hit so the LIVE world never receives the spawn).
	static void DrainHits(World* w);
	// Forget growth/queued spawns of a dying world (called from ~World; nothing dereferenced).
	static void ForgetWorld(World* w);
	static void SaveJson(nlohmann::json& j);         // world "surface" block (omitted when empty)
	static void LoadJson(const nlohmann::json& j);
	static void ResetDefaults();
	static void Register(SurfaceState* s);           // component lifecycle (Init/Destroy)
	static void Unregister(SurfaceState* s);
	static void Register(SurfaceMask* m);
	static void Unregister(SurfaceMask* m);
	static SurfaceState* StateOn(Atom* atom);        // override component on this exact atom
	static const std::vector<SurfaceMask*>& Masks(); // all live masks (renderer/queries)
};

}  // namespace nuke

#endif // !NUKEE_SURFACE_H
