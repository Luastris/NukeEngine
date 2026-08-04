#pragma once
#ifndef NUKEE_SEQUENCEPLAYER_H
#define NUKEE_SEQUENCEPLAYER_H
#include "NukeAPI.h"
#include "Component.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

class Sequence;
class Atom;

// Plays a .nuseq over the world: samples transform/prop/bone tracks every frame, fires clip
// starts, events, camera cuts and audio stings on time crossings. Atom paths resolve from
// THIS component's atom ("" = the atom itself).
class NUKEENGINE_API SequencePlayer : public Component
{
	NUKE_CLASS(SequencePlayer, Component, "Animation")
public:
	[[nuke::prop(asset="sequence", label="Sequence")]] std::string seqGuid;
	[[nuke::prop(label="Play On Start")]] bool playOnStart = true;
	[[nuke::prop(label="Loop")]]          bool loop = false;
	[[nuke::prop(min=0, max=10, label="Speed")]] float speed = 1.0f;

	SequencePlayer();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	[[nuke::func]] void   Play();
	[[nuke::func]] void   Stop();                 // halt + rewind
	[[nuke::func]] void   SetPaused(bool paused);
	[[nuke::func]] bool   IsPlaying();
	[[nuke::func]] double Time();
	[[nuke::func]] void   SetTime(double t);      // scrub (also applies the sampled state)
	// Bake this sequence's BONE tracks into a .nuanim on the subtree's skeleton — keyframe
	// animation authoring. Returns the new clip guid ("" when there are no bone tracks).
	[[nuke::func]] std::string BakeSkeletal(const std::string& outContentRel);

	// Sample every value track + fire crossings in (prevT, t] (editor scrub passes fire=false).
	void ApplyAt(double prevT, double t, bool fire);
	// Drive an explicit in-memory sequence (the editor previews its EDITING copy, which the
	// ResDB instance behind seqGuid knows nothing about).
	void SetSequence(Sequence* s) { seq = s; }

private:
	double t = 0.0;
	bool   playing = false, started = false;
	Sequence* seq = nullptr;              // resolved lazily from seqGuid

	Sequence* EnsureSeq();
	Atom*     ResolvePath(const std::string& path) const;
};

}  // namespace nuke

#endif // !NUKEE_SEQUENCEPLAYER_H
