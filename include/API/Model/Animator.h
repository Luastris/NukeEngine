#pragma once
#ifndef NUKEE_ANIMATOR_H
#define NUKEE_ANIMATOR_H
#include "NukeAPI.h"
#include "Component.h"
#include "AnimClip.h"
#include "Mesh.h"   // MeshBone (bone palette entry)
#include "Vector.h"
#include "reflect/Reflect.h"
#include <map>
#include <string>
#include <vector>

namespace nuke {

class Mesh;
class MeshRenderer;
class SkinnedMeshRenderer;
class Skeleton;
class AnimSM;
class BlendSpace;

// Skeletal animation player next to a skinned MeshRenderer. Two drive modes:
//  * CONTROLLER (smGuid = a .nusm asset): full pose pipeline — parameter-driven states with
//    conditions/exit time/interruption/sub-machines, blend spaces, masked + additive layers,
//    inertialized transitions, sync groups, per-state mirroring and root motion.
//  * LEGACY (no controller): the embedded Play/CrossFade machine; channels also drive matching
//    atom transforms by name, so a mesh is NOT required to Play().
// Clip notifies / float curves / prop tracks fire in BOTH modes.
class NUKEENGINE_API Animator : public Component
{
	NUKE_CLASS(Animator, Component, "Animation")
public:
	[[nuke::prop(asset="animsm", label="Controller", tip="State-machine asset (.nusm). Set = the full pose pipeline drives; empty = the embedded Play/state API.")]] std::string smGuid;
	[[nuke::prop(asset="anim", label="Clip")]]        std::string clipGuid;      // initial clip (controller/entry state wins if set)
	[[nuke::prop(asset="bonemap", label="Bone Map")]] std::string boneMapGuid;   // retarget asset (.nubonemap)
	[[nuke::prop(label="Play On Start")]]        bool  playOnStart = true;
	[[nuke::prop(label="Loop")]]                 bool  loop = true;
	[[nuke::prop(min=0, max=10, label="Speed")]] float speed = 1.0f;
	[[nuke::prop(label="Root Motion", tip="Extract the root bone's horizontal travel + yaw from the pose and move the atom by it.")]] bool rootMotion = false;
	// Serialized state machine: {"states":{name:{clip,loop,speed}},"transitions":[...],"entry":...}.
	[[nuke::prop(hidden)]] std::string smJson;

	Animator();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	// --- script surface (auto-bound) ---
	[[nuke::func]] void Play(const std::string& clip);                    // guid OR clip name
	[[nuke::func]] void CrossFade(const std::string& clip, double fade);  // fade seconds
	[[nuke::func]] void Stop();
	[[nuke::func]] bool IsPlaying();
	[[nuke::func]] std::string CurrentClip();                             // playing clip's NAME ("" = none)
	[[nuke::func]] double ClipTime();
	[[nuke::func]] void SetClipTime(double t);
	// State machine — serialized with the component (smJson); auto-enters `entry` on play.
	[[nuke::func]] void AddState(const std::string& name, const std::string& clip, bool stateLoop, double stateSpeed);
	[[nuke::func]] void RemoveState(const std::string& name);
	[[nuke::func]] void AddTransition(const std::string& from, const std::string& to, double fade);
	[[nuke::func]] void RemoveTransition(const std::string& from, const std::string& to);
	[[nuke::func]] void SetEntry(const std::string& name);                // state entered on play start
	[[nuke::func]] std::string Entry();
	[[nuke::func]] void SetState(const std::string& name);                // cross-fades if a transition matches
	[[nuke::func]] std::string State();
	// Add a marker to a clip (guid or name); fired as Component::OnAnimEvent on siblings.
	[[nuke::func]] void AddEvent(const std::string& clip, double t, const std::string& name);
	// Retarget: rename clip channels onto this skeleton's bone names, for every clip played.
	[[nuke::func]] void MapBone(const std::string& from, const std::string& to);
	[[nuke::func]] void ClearBoneMap();
	// IK post-pass on the sampled pose: pull `tipBone` toward a WORLD-space target, weight
	// [0..1] blends against the clip. Chain 2 = analytic two-bone, more = FABRIK; the pole
	// is a WORLD-space point aiming the bend plane.
	[[nuke::func]] void SetIK(const std::string& tipBone, const Vector3& target, double weight);
	[[nuke::func]] void SetIKPole(const std::string& tipBone, const Vector3& pole);
	[[nuke::func]] void SetIKChain(const std::string& tipBone, double segments);
	[[nuke::func]] void ClearIK(const std::string& tipBone);
	// Same solver driven by a NAMED CHAIN from the skeleton's IK rig (.nuskel `chains`,
	// root -> tip) — no bone names or segment counts at the call site. The optional NORMAL
	// aligns the chain tip's up-axis to a world surface normal after the reach (foot roll).
	[[nuke::func]] void SetChainIK(const std::string& chain, const Vector3& target, double weight);
	[[nuke::func]] void SetChainIKPole(const std::string& chain, const Vector3& pole);
	[[nuke::func]] void SetChainIKNormal(const std::string& chain, const Vector3& normal, double weight);
	[[nuke::func]] void ClearChainIK(const std::string& chain);
	// Look-at: turn a weighted joint chain (spine -> head, from the IK rig) so the tip's
	// forward aims at a WORLD target; the turn spreads along the chain growing toward the
	// tip, capped at maxAngle degrees. A bare bone name works as a one-joint chain.
	[[nuke::func]] void SetLookAt(const std::string& chain, const Vector3& target, double weight, double maxAngle);
	[[nuke::func]] void ClearLookAt();
	// Vertical hips offset in world units, applied to the common ancestor of the active IK
	// chains (FootIK lowers the pelvis so every foot can reach the ground).
	[[nuke::func]] void SetPelvisOffset(double dy);
	// The resolved skeleton asset driving this Animator (null before the first Update).
	Skeleton* CurrentSkeleton() const;
	// Controller parameters (smGuid mode). Triggers auto-reset when a transition consumes them.
	[[nuke::func]] void   SetFloat(const std::string& param, double v);
	[[nuke::func]] double GetFloat(const std::string& param);
	[[nuke::func]] void   SetBool(const std::string& param, bool v);
	[[nuke::func]] bool   GetBool(const std::string& param);
	[[nuke::func]] void   SetTrigger(const std::string& param);
	[[nuke::func]] void   ResetTrigger(const std::string& param);
	// Blended value of a named clip float curve this frame (0 when no active clip carries it).
	[[nuke::func]] double CurveValue(const std::string& curve);
	// Controller layers: current state name + runtime weight override (index = .nusm order).
	[[nuke::func]] std::string LayerState(double layer);
	[[nuke::func]] void SetLayerWeight(double layer, double weight);
	// Root motion applied to the atom LAST frame (world units) — feed a CharacterController.
	[[nuke::func]] Vector3 RootDelta();
	// Runtime whole-pose mirror override on top of the per-state flags.
	[[nuke::func]] void SetMirror(bool mirrored);
	// Motion-matching jump: play `clipRef` FROM `time` with an inertialized blend over `blend`
	// seconds (no pop, the old stream is never sampled again). Legacy drive mode.
	[[nuke::func]] void MatchTo(const std::string& clipRef, double time, double blend);
	// Model-space bone globals (16 floats each, column-major) of a clip sampled at `time` over
	// the CURRENT skeleton — offline feature extraction (motion matching DB). False when the
	// skeleton is unresolved.
	bool SamplePoseGlobals(AnimClip* clip, double time, std::vector<float>& outGlobals);

private:
	// One playing clip layer: time cursor + per-channel bone mapping.
	struct Layer
	{
		AnimClip* clip = nullptr;
		double t = 0.0;
		bool   loop = true;
		double speed = 1.0;
		std::vector<int> boneMap;   // channel index -> mesh bone index (-1 = no such bone)
	};
	struct StateDef { std::string clip; bool loop = true; double speed = 1.0; };

	Layer  cur, prev;
	double fadeLeft = 0.0, fadeDur = 0.0;
	bool   playing = false, started = false;

	MeshRenderer* mr = nullptr;        // sibling renderer (resolved lazily)
	// NEW pipeline targets: EVERY SkinnedMeshRenderer in the atom's SUBTREE sharing one
	// skeleton asset (modular characters — the Animator sits on the prefab root, the meshes
	// on child nodes). `smr` = the first one (sockets/masks/notifies read through it).
	SkinnedMeshRenderer* smr = nullptr;
	std::vector<SkinnedMeshRenderer*> smrs;
	// Bone palette sampling/IK works over: the SKELETON asset bones (SMR path) or the legacy
	// embedded srcMesh->bones. Set by EnsureTargets.
	const std::vector<MeshBone>* bonesRef = nullptr;
	Mesh* srcMesh = nullptr;           // bind-pose source asset
	Mesh* skinnedMesh = nullptr;       // instance-owned dynamic copy the renderer draws (LEGACY path only)

	struct IKGoal
	{
		float  target[3];
		double weight = 1.0;
		bool   hasPole = false;
		float  pole[3] = { 0, 0, 0 };
		int    segments = 2;   // 2 = analytic two-bone; >2 = FABRIK over that many segments
		// Chain-goal extras (SetChainIK): the map key names a skeleton IK-rig chain instead
		// of a tip bone; the optional normal re-aims the tip's up-axis after the reach.
		bool   isChain = false;
		bool   hasNormal = false;
		float  normal[3] = { 0, 1, 0 };
		double normalWeight = 1.0;
	};

public:   // the editor's Animator window edits the deserialized machine directly
	std::map<std::string, StateDef> states;
	std::map<std::string, std::map<std::string, double>> transitions;   // from -> to -> fade
	std::string entryState;
	void EnsureSM();          // decode smJson once (call before touching states/transitions)
	void EncodeSM();          // states/transitions/entry -> smJson (called by every mutator)

	// Editor preview seam: a live EDITING copy overrides the ResDB lookup for the controller
	// and/or one blend space (SequencePlayer::SetSequence pattern). Setting drops the runtime
	// graph — the next Update rebinds cleanly against the edited structure.
	AnimSM*     previewSm = nullptr;
	BlendSpace* previewBlend = nullptr;
	AnimClip*   previewClip = nullptr;   // ResolveClip serves this copy for its guid/name
	void SetPreviewController(AnimSM* sm, BlendSpace* blend = nullptr);
	// Preview rigs live in aux worlds: notifies must not spawn prefabs / play sounds / shake
	// cameras in the LIVE world (Prefabs::Spawn and the event bus are global).
	bool muteNotifies = false;

private:
	std::string curState;
	bool smLoaded = false;
	std::map<std::string, std::string> boneMap;    // runtime renames (override the bonemap asset)
	std::map<std::string, IKGoal>      ikGoals;    // tip bone -> world-space goal

	// Controller-mode runtime (states, params, layer poses, inertializers) — owned, opaque to
	// keep the exported layout stable while the pipeline grows.
	struct AnimGraph;
	AnimGraph* graph = nullptr;
	void ForceGraphState(const std::string& name);   // controller-mode SetState
	std::string GraphStateName();                    // controller-mode State()
	Vector3 lastRootDelta;
	bool mirrorOverride = false;
	// SpawnPrefab notifies with a lifetime: (atom id, game-time deadline) -> QueueDestroy.
	std::vector<std::pair<long, double>> notifySpawns;

	// transform (node) animation: current clip's channel -> target atom (by name)
	std::vector<Atom*> channelAtoms;
	AnimClip* atomBindClip = nullptr;
	std::map<std::string, int> prevChanByBone;   // outgoing clip: bone name -> channel (fade blending)

	AnimClip* ResolveClip(const std::string& ref) const;   // guid first, then name
	std::string MapName(const std::string& boneName) const;// runtime map > bonemap asset > as-is
	void  BindLayer(Layer& l) const;                       // resolve names against the mesh skeleton
	void  BindAtoms();                                     // resolve channels against the atom tree
	void  ApplyAtomAnimation();                            // write sampled TRS to matched atoms
	bool  EnsureTargets();                                 // mr / srcMesh / skinnedMesh
	void  StartClip(AnimClip* c, bool clipLoop, double clipSpeed, double fade);
	void  ReleaseSkinned();
};

}  // namespace nuke

#endif // !NUKEE_ANIMATOR_H
