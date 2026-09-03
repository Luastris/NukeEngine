#pragma once
#ifndef NUKEE_CLOTH_H
#define NUKEE_CLOTH_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <string>
#include <vector>

namespace nuke {

class Atom;
class Mesh;
class MeshRenderer;
class SkinnedMeshRenderer;
class iPhysics;

// C3: soft-body cloth over the atom's rendered mesh (flags, capes, skirts, tablecloths).
// The mesh's UV-seam duplicates are WELDED into one sim sheet, simulated by the physics
// backend inside the world's fixed step (Physics::Scene() routing — the prefab editor's
// sandbox works unchanged), and written back into a private render instance every step.
// Pins follow the animation: a skinned mesh pins by bone weights (skirt waistband), a
// plain mesh by its top edge (flag on a pole); pinned vertices ride the pose, the rest
// hang, collide with the scene's bodies AND with the character's .nurag capsules, and
// catch the global nuke::Wind.
class NUKEENGINE_API Cloth : public Component
{
	NUKE_CLASS(Cloth, Component, "Physics")
public:
	[[nuke::prop(enum="None,Top Edge,Bone Weights", label="Pin Mode", tip="Which vertices are sewn to the animation: the mesh's top band (flags, capes) or the vertices skinned to Pin Bones (a skirt's waistband). None = fully free (tablecloth)")]]
	int pinMode = 1;
	[[nuke::prop(label="Pin Bones", tip="';'-separated bone names for Bone Weights mode - a vertex with at least half its skin weight on them is pinned")]]
	std::string pinBones;
	[[nuke::prop(label="Pin Band", min=0, max=1, tip="Top Edge mode: fraction of the mesh height (from the top) that pins")]]
	float pinBand = 0.15f;
	[[nuke::prop(label="Stiffness", min=0, max=1, tip="Stretch resistance of the sheet; 1 = inextensible")]]
	float stiffness = 0.9f;
	[[nuke::prop(label="Bend Stiffness", min=0, max=1, tip="Fold resistance; low = silky, high = leathery")]]
	float bendStiffness = 0.2f;
	[[nuke::prop(label="Thickness", min=0, tip="Collision skin around every vertex")]]
	float thickness = 0.01f;
	[[nuke::prop(label="Friction", min=0, max=1)]]
	float friction = 0.3f;
	[[nuke::prop(label="Damping", min=0, tip="Velocity kill per second; low = flowy")]]
	float damping = 0.1f;
	[[nuke::prop(label="Gravity Factor", tip="Multiplier on scene gravity")]]
	float gravityFactor = 1.0f;
	[[nuke::prop(label="Pressure", min=0, tip="Closed meshes only: internal pressure (balloons)")]]
	float pressure = 0.0f;
	[[nuke::prop(label="Iterations", min=1, max=32, tip="Solver iterations per step - more = stiffer under load, costlier")]]
	int iterations = 5;
	[[nuke::prop(label="Wind", tip="Catch the global wind and WindZones")]]
	bool windOn = true;
	[[nuke::prop(label="Body Collision", tip="Push the sheet out of the atom's ragdoll (.nurag) capsules; scene bodies always collide")]]
	bool collision = true;
	[[nuke::prop(label="Weld Distance", min=0, tip="UV-seam duplicates closer than this merge into one sim vertex")]]
	float weldDistance = 0.001f;
	[[nuke::prop(label="Max Distance", min=0, tip="Skinned meshes: a free vertex may stray at most this far from its ANIMATED (skinned) position - the fitted-clothes leash (skirts, sleeves). 0 = fully free (capes)")]]
	float maxDistance = 0.15f;
	[[nuke::prop(label="Backstop", min=0, tip="Skinned meshes: a vertex may sink at most this far BEHIND its skinned surface (face-normal backstop) - keeps fitted cloth out of the body. Near zero for tight clothes")]]
	float backstop = 0.005f;
	[[nuke::prop(label="Body Gap", min=0, tip="Skinned meshes: the sheet is inflated this far along its normals - fitted cloth hugs an air cushion instead of the skin, so a flexing thigh can't poke through")]]
	float bodyGap = 0.01f;
	[[nuke::prop(label="Sim Band", min=0, max=1, tip="Skinned meshes: only the BOTTOM fraction of the sheet simulates, the rest rides the animation rigidly (skirt hems, coat tails - the fitted part can never be pushed through by the body). 1 = the whole sheet simulates")]]
	float simBand = 1.0f;

	// Children tick before their parent, so Update sees LAST frame's animator globals — the
	// garment edge would lag the top by a frame. The late pass re-writes the sheet with the
	// fresh globals.
	void LateUpdate() override;

	// Drop the sim and rebuild from the current pose (after prop edits that change the sheet).
	[[nuke::func]] void Rebuild();
	// --- script surface (auto-bound) ---
	[[nuke::func]] double  SimVertexCount();   // welded sim vertices (0 = not built yet)
	[[nuke::func]] double  PinnedCount();
	[[nuke::func]] Vector3 CenterOfMass();     // world center of the simulated sheet

	Cloth() : Component("Cloth") {}
	~Cloth();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;            // render write: sim interpolated between fixed steps
	void FixedUpdate() override;
	void Pause() override {}
	void Reset() override { Rebuild(); }

private:
	bool EnsureBody();
	void ReleaseBody();
	void GatherPins(std::vector<float>& outWorld);      // world targets, pin order
	void ComputeBodyCaps();                             // full-size body capsules, auto-fit to the flesh (bind pose)
	void BuildBodyProxies();                            // those capsules -> kinematic softOnly bodies
	void DriveBodyProxies();                            // ride the pose, every fixed step
	void WriteRenderMesh(const float* simPos, const float anchor[3]);   // sim -> m_clothMesh (atom-local)

	iPhysics* m_scene = nullptr;       // the scene the body lives in (Physics::Scene() at build)
	uint64_t  m_body = 0;
	Mesh*     m_srcMesh = nullptr;     // bind-pose asset
	Mesh*     m_clothMesh = nullptr;   // owned render instance (pos/nrm own, rest shared)
	SkinnedMeshRenderer* m_smr = nullptr;   // set when the renderer is skinned
	std::vector<int>   m_renderToSim;  // render vertex -> sim vertex
	std::vector<int>   m_simToRender;  // sim vertex -> FIRST render vertex (skin sampling)
	bool m_skinnedMode = false;        // Jolt skinned constraints drive the fit (SMR path)
	std::vector<float> m_joints16;     // scratch: world joint matrices fed each step
	std::vector<float> m_invMass;      // per sim vertex (0 = pinned)
	std::vector<int>   m_pinIdx;       // sim indices of the pinned vertices
	std::vector<unsigned int> m_simTris;
	std::vector<float> m_simPos;       // scratch: world positions after the last step
	std::vector<float> m_effInvBind;   // 16 per bone: the mesh's own binds override (SMR)
	// Body proxies: the ragdoll's capsules as kinematic softOnly bodies (the solver keeps
	// the sheet out of the body; invisible to the rest of the scene).
	struct BodyProxy { uint64_t body; int bone; float center[3], axis[3], halfHeight; };
	std::vector<BodyProxy> m_proxies;
	// Render clip capsules: the ragdoll's capsules at FULL size. The final render write
	// clamps every vertex OUTSIDE them — whatever the sim does, clothing never draws
	// inside the body.
	struct ClipCap { int bone; float center[3], axis[3], halfHeight, radius; };
	std::vector<ClipCap> m_clipCaps;
	// Render backstop: the sheet's own (inflated) bind positions + normals, skinned per
	// frame — a drawn vertex may never sink BEHIND its skinned surface. Gap-free coverage
	// where capsule seams (groin, glutes) leak.
	std::vector<float> m_bindPos;      // 3 per sim vertex, inflated bind
	std::vector<float> m_bindNrm;      // 3 per sim vertex, bind normals
	// Per-vertex FLESH reference: the most protruding body vertex under each sheet vertex
	// (bound once, in bind pose). Per frame it skins with ONE joint matrix and the drawn
	// sheet is clamped above it — pose-accurate body clipping, no capsule seams.
	std::vector<int>   m_fleshBone;    // per sim vertex; -1 = no body under it
	std::vector<float> m_fleshLocal;   // 3 per sim vertex, in that bone's space
	std::vector<char>  m_hardVert;     // per sim vertex: hard-skinned (pin / above Sim Band) —
	                                   // drawn EXACTLY as authored, no gap, no clips: the
	                                   // waistband must merge seamlessly with the top
	// Anchor-space sim (skinned mode): the sheet simulates around the skeleton's mean joint
	// position (translation only) — world travel, clip drift and loop teleports subtract out
	// before the solver ever sees them. prev/cur anchor pair feeds the render interpolation.
	float m_prevAnchor[3] = { 0, 0, 0 };
	float m_curAnchor[3] = { 0, 0, 0 };
	bool  m_haveAnchor = false;
	// Fixed-step interpolation: the render sheet lerps prev -> cur by the wall-clock phase.
	std::vector<float> m_prevPos;      // sim positions at the PREVIOUS fixed step
	std::vector<float> m_lerpPos;      // scratch for the interpolated write
	double m_lastFixSec = 0.0;         // steady-clock stamp of the last fixed step
	double m_fixDtSec = 1.0 / 60.0;    // measured fixed cadence
	bool   m_havePrev = false;
	int  m_numSim = 0;
	bool m_failed = false;             // bad setup: stop retrying every step
};

}  // namespace nuke

#endif // !NUKEE_CLOTH_H
