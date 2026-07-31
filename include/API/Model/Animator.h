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

// Skeletal animation player next to a skinned MeshRenderer: samples the current clip (cross-fading
// from the previous), builds the bone palette and skins the mesh on the job pool. Channels also
// drive matching atom transforms by name, so a mesh is NOT required to Play().
class NUKEENGINE_API Animator : public Component
{
	NUKE_CLASS(Animator, Component, "Animation")
public:
	[[nuke::prop(asset="anim", label="Clip")]]        std::string clipGuid;      // initial clip (entry state wins if set)
	[[nuke::prop(asset="bonemap", label="Bone Map")]] std::string boneMapGuid;   // retarget asset (.nubonemap)
	[[nuke::prop(label="Play On Start")]]        bool  playOnStart = true;
	[[nuke::prop(label="Loop")]]                 bool  loop = true;
	[[nuke::prop(min=0, max=10, label="Speed")]] float speed = 1.0f;
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

public:   // the editor's Animator window edits the deserialized machine directly
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
