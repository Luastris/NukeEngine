#pragma once
#ifndef NUKEE_RAGDOLL_H
#define NUKEE_RAGDOLL_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <map>
#include <string>
#include <vector>

namespace nuke {

class Skeleton;
class SkinnedMeshRenderer;
class Animator;

// Ragdoll ASSET (.nurag, JSON): per-bone capsules + SwingTwist joint limits for ONE skeleton.
// AUTO-GENERATED at import from the skin weights (capsule fit) — the Unity-checkbox
// experience; hand-tune in the text editor when needed.
class NUKEENGINE_API RagdollDef
{
	NUKE_CLASS(RagdollDef, Object)
public:
	std::string guid;                          // asset id (ResDB)
	[[nuke::prop(label="Name")]] std::string name;
	std::string skelGuid;                      // the skeleton this rig fits

	struct Body
	{
		std::string bone;
		float center[3] = { 0, 0, 0 };         // capsule center, BONE-local
		float axis[3]   = { 0, 1, 0 };         // capsule axis, BONE-local (normalized)
		float radius = 0.05f;
		float halfHeight = 0.1f;               // cylinder half-length
		float mass = 1.0f;
	};
	struct Joint                                // child body -> its nearest bodied ancestor
	{
		std::string bone;                       // child body's bone
		float twistMin = -0.6f, twistMax = 0.6f;   // radians
		float swing1 = 0.8f, swing2 = 0.8f;        // half-cone angles (radians)
	};
	std::vector<Body>  bodies;
	std::vector<Joint> joints;

	// Auto-fit from skin weights: a capsule per sufficiently-weighted bone (axis toward the
	// dominant child / vertex spread), default humanoid-ish limits. Total mass distributed
	// by capsule volume. Returns null when the mesh has no usable skin.
	static RagdollDef* Build(const Skeleton* sk, const class Mesh* mesh, float totalMass = 70.0f);

	// Native asset format (.nurag): JSON — small, diff-able, hand-editable.
	bool              SaveToFile(const std::string& path) const;
	static RagdollDef* LoadFromFile(const std::string& path);
	static RagdollDef* LoadFromMemory(const std::string& data);
	static RagdollDef* FromString(const std::string& json);
	std::string       ToString() const;
};

// Runtime ragdoll next to the Animator/SkinnedMeshRenderer. The Unity checkbox: add the
// component — the .nurag auto-resolves from the skeleton; switch Mode to drop.
//   Off     — pure animation.
//   Full    — physics owns every bodied bone; the pose is read back from the simulation.
//   Powered — joint motors chase the ANIMATED pose (hit reactions); physics writes back.
//   Partial — Full, but only the subtree from `partialRoot`; the rest stays animated.
class NUKEENGINE_API Ragdoll : public Component
{
	NUKE_CLASS(Ragdoll, Component, "Animation")
public:
	enum class Mode { Off = 0, Full = 1, Powered = 2, Partial = 3 };
	static constexpr bool reflected = true;

	[[nuke::prop(asset="ragdoll", label="Ragdoll", tip="Empty = auto-resolve the .nurag matching the skeleton.")]] std::string ragGuid;
	[[nuke::prop(label="Mode", enum="Off,Full,Powered,Partial")]] Mode mode = Mode::Off;
	[[nuke::prop(min=0, max=1, label="Blend", tip="Physics pose weight while active.")]] float blend = 1.0f;
	[[nuke::prop(min=0, max=60, label="Motor Frequency", tip="Powered: joint spring frequency, Hz.")]] float motorFrequency = 15.0f;
	[[nuke::prop(min=0, max=5, label="Motor Damping")]] float motorDamping = 1.0f;
	[[nuke::prop(label="Partial Root", tip="Partial mode: physics owns this bone's subtree.")]] std::string partialRoot;
	[[nuke::prop(min=1, max=500, label="Total Mass")]] float totalMass = 70.0f;

	Ragdoll();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	[[nuke::func]] void SetMode(double m);          // 0 Off / 1 Full / 2 Powered / 3 Partial
	[[nuke::func]] double GetMode();
	[[nuke::func]] void Impulse(const std::string& bone, const Vector3& worldImpulse);

	// Blends the physics pose into smr->pose (and feeds the animated pose to the motors in
	// Powered mode). The Animator calls it at handover with apply=false (its own ApplyPose
	// follows — one skin per frame); standalone SMRs are driven from Update with apply=true.
	void BlendInto(SkinnedMeshRenderer* smr, bool apply = true);
	bool Active() const { return active; }

private:
	struct BodyRt { int bone = -1; uint64_t body = 0; float invOffRot[4]; };
	std::vector<BodyRt> bodiesRt;
	std::vector<uint64_t> jointsRt;
	std::vector<int> jointBone, jointParentBone;
	RagdollDef* def = nullptr;
	bool active = false;
	bool fed = false;      // an Animator handed us the pose this frame (skip self-drive)
	Mode liveMode = Mode::Off;

	RagdollDef* EnsureDef(SkinnedMeshRenderer* smr);
	void Activate(SkinnedMeshRenderer* smr);
	void Deactivate();
	bool InPartial(const Skeleton* sk, int bone) const;
};

}  // namespace nuke

#endif // !NUKEE_RAGDOLL_H
