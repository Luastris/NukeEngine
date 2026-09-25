#pragma once
#ifndef NUKEE_SPRINGBONES_H
#define NUKEE_SPRINGBONES_H
#include "NukeAPI.h"
#include "Component.h"
#include "Mesh.h"   // MeshBone
#include "reflect/Reflect.h"
#include <string>
#include <vector>

namespace nuke {

class Atom;
class Skeleton;

// The local TRS slice the spring solver reads/writes (r = quat xyzw). CommitPose converts
// its pose to/from this so the solver stays out of Animator internals.
struct SpringPose
{
	float p[3];
	float s[3];
	float r[4];
};

// One frame of SHARED solve context for a whole spring group: animated model-space bone
// globals + the atom's ragdoll capsules in world space. Build once per atom per frame
// (SpringBones::BuildSolveCtx) and hand to every chain's Apply — per-chain rebuilds of a
// 300-bone forward pass and name-matched capsule lists made many-chain rigs crawl.
struct SpringSolveCtx
{
	std::vector<float> global;   // 16 floats per bone, model space
	struct Capsule { float a[3]; float b[3]; float r; int bone; };
	std::vector<Capsule> caps;   // world space; bone = skeleton index (chains exclude their own)
};

// C4: a damped spring simulation over ONE bone chain, on TOP of the finished animated pose —
// hair cards, tails, breast/jiggle, loose accessories. Solved in Animator::CommitPose order
// (after IK/look-at and the history snapshot, before the skin handover), in WORLD space so
// character motion drives the lag naturally. Colliders come from the atom's .nurag capsules.
// The wave driver layers procedural secondary motion (tail wag / wing flap / sway) onto
// authored clips — the spring smooths the wave into the chain. One component per chain.
class NUKEENGINE_API SpringBones : public Component
{
	NUKE_CLASS(SpringBones, Component, "Animation")
public:
	[[nuke::prop(label="Chain", tip="Rig chain name on the skeleton (Tail/LeftArm/...), or a bone name - the chain then follows first children down to the leaf")]]
	std::string chain;
	[[nuke::prop(label="Stiffness", min=0, max=1, tip="Pull back toward the animated pose per second-ish; low = floppy")]]
	float stiffness = 0.12f;
	[[nuke::prop(label="Damping", min=0, max=1, tip="Velocity kill per step; low = bouncy")]]
	float damping = 0.2f;
	[[nuke::prop(label="Gravity", tip="m/s^2 downward on the chain tails (droop)")]]
	float gravity = 0.0f;
	[[nuke::prop(label="Wind", tip="Chain reacts to the global wind and WindZones (sampled per segment: gusts and turbulence ripple along it); off for heavy chains")]]
	bool windOn = true;
	[[nuke::prop(label="Collision", tip="Push the chain out of the atom's ragdoll (.nurag) capsules")]]
	bool collision = true;
	[[nuke::prop(label="Radius", min=0, tip="Chain thickness for the capsule collision")]]
	float radius = 0.03f;
	[[nuke::prop(label="Wave Amplitude", min=0, tip="Procedural wag/flap/sway on top of the clip, degrees; 0 = off")]]
	float waveDeg = 0.0f;
	[[nuke::prop(label="Wave Hz", min=0)]]
	float waveHz = 1.0f;
	[[nuke::prop(label="Wave Axis", enum="Yaw,Pitch,Roll", tip="Local bone axis the wave rotates around (Yaw = wag, Pitch = flap)")]]
	int waveAxis = 0;
	[[nuke::prop(label="Wave Travel", tip="Degrees of phase per joint - the wave RUNS along the chain")]]
	float waveTravel = 60.0f;

	// One frame of spring solve over `pose` (local TRS per bone); true = rotations changed.
	// `model` = the atom's world matrix (column-major), `sk` may be null (bone-name chains).
	// `ctx` = the group's shared solve context (BuildSolveCtx); null falls back to a private
	// rebuild — fine for a single chain, ruinous for dozens.
	bool Apply(const std::vector<MeshBone>& bones, const Skeleton* sk, SpringPose* pose,
	           int nb, const float model[16], Atom* owner, double dt,
	           const SpringSolveCtx* ctx = nullptr);

	// Shared per-frame context for the atom's whole spring group: forward pass over `pose`
	// + (wantCaps) the atom's ragdoll capsules in world space.
	static void BuildSolveCtx(const std::vector<MeshBone>& bones, const Skeleton* sk,
	                          const SpringPose* pose, int nb, const float model[16],
	                          Atom* owner, bool wantCaps, SpringSolveCtx& out);

	[[nuke::func]] void Reset();   // drop the sim state (teleports re-prime automatically too)

	// Springs are PHYSICS: they simulate every frame ON THEIR OWN (Update self-drives the
	// atom's whole spring group over the subtree SMRs' pose palettes). A committing Animator
	// still owns the solve while it plays — CommitPose layers springs after IK and marks the
	// frame here, and Update backs off (lastSolveByAnim keeps deferring one extra frame so
	// component order between the Animator and the springs never double-steps the sim).
	unsigned long long lastSolveFrame = 0;   // Time frame of the last Apply (any driver)
	bool lastSolveByAnim = false;            // that Apply came from Animator::CommitPose

	SpringBones() : Component("SpringBones") {}
	void Init(Atom* parent) override;
	void Destroy() override {}
	void Update() override;
	TimeDomain timeDomain() const override { return TimeDomain::Animation; }
	void FixedUpdate() override {}
	void Pause() override {}

private:
	// chain resolve cache (invalidated when the palette or the prop changes)
	std::vector<int> chainIdx;
	std::string cachedChain;
	size_t cachedBones = 0;
	// verlet state: world tail per segment
	std::vector<float> cur, prev;
	bool primed = false;
};

}  // namespace nuke

#endif // !NUKEE_SPRINGBONES_H
