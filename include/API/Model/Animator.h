#pragma once
#ifndef NUKEE_ANIMATOR_H
#define NUKEE_ANIMATOR_H
#include "NukeAPI.h"
#include "Component.h"
#include "AnimClip.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <map>
#include <string>
#include <vector>

namespace nuke {

class Mesh;
class MeshRenderer;

// Skeletal animation player (roadmap 3.1). Sits NEXT TO the skinned MeshRenderer: each
// frame it samples the current clip (cross-fading from the previous one), builds the
// bone palette and skins the mesh on the SHARED job pool (nuke::Jobs, 2.4) into an
// instance-owned dynamic mesh which the MeshRenderer then draws — every existing pass
// (lit / gbuffer / shadows / outline, any custom shader) renders it with no special
// pipeline. RT reflections use the bind-pose source (Mesh::rtProxy; per-frame BLAS
// rebuilds are a non-goal).
//
// TRANSFORM (node) animation: independently of skinning, channels also drive matching
// ATOM transforms by name (searched from the Animator's ROOT ancestor) — a skeleton
// prefab without any mesh animates its joint atoms, a DCC door/elevator just moves.
// The retarget map applies to atom matching too. A mesh is NOT required to Play().
//
// Clips address bones BY NAME (see AnimClip). A minimal STATE MACHINE is built at
// runtime from scripts: AddState/AddTransition/SetState (a transition's fade drives the
// cross-fade); Play/CrossFade work directly with clip guids or names as well.
class NUKEENGINE_API Animator : public Component
{
	NUKE_CLASS(Animator, Component)
public:
	[[nuke::prop(asset="anim", label="Clip")]]        std::string clipGuid;      // initial clip (entry state wins if set)
	[[nuke::prop(asset="bonemap", label="Bone Map")]] std::string boneMapGuid;   // retarget asset (.nubonemap)
	[[nuke::prop(label="Play On Start")]]        bool  playOnStart = true;
	[[nuke::prop(label="Loop")]]                 bool  loop = true;
	[[nuke::prop(min=0, max=10, label="Speed")]] float speed = 1.0f;
	// Serialized state machine (hidden: edited via the Animator window / script API, saved
	// with the world/prefab): {"states":{name:{clip,loop,speed}},"transitions":[...],"entry":...}.
	[[nuke::prop(hidden)]] std::string smJson;

	Animator();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	// --- script surface (auto-bound: comp:Play("walk") ...) ----------------------------
	[[nuke::func]] void Play(const std::string& clip);                    // guid OR clip name
	[[nuke::func]] void CrossFade(const std::string& clip, double fade);  // fade seconds
	[[nuke::func]] void Stop();
	[[nuke::func]] bool IsPlaying();
	[[nuke::func]] std::string CurrentClip();                             // playing clip's NAME ("" = none)
	[[nuke::func]] double ClipTime();
	[[nuke::func]] void SetClipTime(double t);
	// state machine — SERIALIZED with the component (smJson); build it in the editor's
	// Animator window or from scripts, either way it persists and auto-enters `entry`.
	[[nuke::func]] void AddState(const std::string& name, const std::string& clip, bool stateLoop, double stateSpeed);
	[[nuke::func]] void RemoveState(const std::string& name);
	[[nuke::func]] void AddTransition(const std::string& from, const std::string& to, double fade);
	[[nuke::func]] void RemoveTransition(const std::string& from, const std::string& to);
	[[nuke::func]] void SetEntry(const std::string& name);                // state entered on play start
	[[nuke::func]] std::string Entry();
	[[nuke::func]] void SetState(const std::string& name);                // cross-fades if a transition matches
	[[nuke::func]] std::string State();
	// animation events: fired as Component::OnAnimEvent on every sibling component when
	// the playhead crosses the marker (loop-aware). AddEvent targets a clip (guid or name).
	[[nuke::func]] void AddEvent(const std::string& clip, double t, const std::string& name);
	// retargeting: rename clip channels onto this skeleton's bone names (e.g.
	// MapBone("mixamorig:Hips", "Hips")); applies to every clip this Animator plays.
	[[nuke::func]] void MapBone(const std::string& from, const std::string& to);
	[[nuke::func]] void ClearBoneMap();
	// IK (post-pass on the sampled pose): pull `tipBone` toward a WORLD-space target;
	// weight [0..1] blends against the clip. Default chain = 2 segments (tip<-mid<-root,
	// analytic); SetIKChain lengthens it (FABRIK). SetIKPole aims the bend plane at a
	// WORLD-space pole (elbow/knee direction control).
	[[nuke::func]] void SetIK(const std::string& tipBone, const Vector3& target, double weight);
	[[nuke::func]] void SetIKPole(const std::string& tipBone, const Vector3& pole);
	[[nuke::func]] void SetIKChain(const std::string& tipBone, double segments);
	[[nuke::func]] void ClearIK(const std::string& tipBone);

private:
	// One playing clip layer: time cursor + per-channel bone mapping (resolved per mesh).
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
	Mesh* srcMesh = nullptr;           // bind-pose source asset
	Mesh* skinnedMesh = nullptr;       // instance-owned dynamic copy the renderer draws

	struct IKGoal
	{
		float  target[3];
		double weight = 1.0;
		bool   hasPole = false;
		float  pole[3] = { 0, 0, 0 };
		int    segments = 2;   // 2 = analytic two-bone; >2 = FABRIK over that many segments
	};

public:   // the editor's Animator window edits the (deserialized) machine directly
	std::map<std::string, StateDef> states;
	std::map<std::string, std::map<std::string, double>> transitions;   // from -> to -> fade
	std::string entryState;
	void EnsureSM();          // decode smJson once (call before touching states/transitions)
	void EncodeSM();          // states/transitions/entry -> smJson (called by every mutator)

private:
	std::string curState;
	bool smLoaded = false;
	std::map<std::string, std::string> boneMap;    // runtime renames (override the bonemap asset)
	std::map<std::string, IKGoal>      ikGoals;    // tip bone -> world-space goal

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
