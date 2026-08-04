#pragma once
#ifndef NUKEE_SEQUENCE_H
#define NUKEE_SEQUENCE_H
#include "NukeAPI.h"
#include "AnimClip.h"   // Key (shared key format with clips)
#include <string>
#include <vector>
#include "reflect/Reflect.h"

namespace nuke {

// Sequence ASSET (.nuseq, JSON): world orchestration on a timeline — atom transforms, ANY
// reflected prop, skeleton bones, anim clips, events, camera cuts and audio stings. Atom
// paths are relative to the SequencePlayer's atom ("" = the atom itself, "A/B" descends).
// Keys reuse AnimClip::Key (t + float4); rotations are quats (x,y,z,w).
class NUKEENGINE_API Sequence
{
	NUKE_CLASS(Sequence, Object)
public:
	std::string guid;                          // asset id (ResDB)
	[[nuke::prop(label="Name")]]     std::string name;
	[[nuke::prop(label="Duration")]] double duration = 5.0;   // seconds

	struct TransformTrack
	{
		std::string path;
		std::vector<AnimClip::Key> pos, rot, scale;   // any may be empty
	};
	struct PropTrack                            // = AnimClip::PropTrack + atom path semantics
	{
		std::string path, comp, prop;
		int dim = 1;                            // 1..4 float components; 0 = bool step
		std::vector<AnimClip::Key> keys;
	};
	struct BoneTrack                            // keyframe skeletal authoring (bakes to .nuanim)
	{
		std::string path;                       // atom carrying the SkinnedMeshRenderer
		std::string bone;
		std::vector<AnimClip::Key> pos, rot;
	};
	struct ClipEntry { float t = 0; std::string clip; float speed = 1; };
	struct ClipTrack                            // fire Animator clips on an atom
	{
		std::string path;
		std::vector<ClipEntry> entries;
	};
	struct Event  { float t = 0; std::string name, payload; };   // Events::Emit
	struct Cut    { float t = 0; std::string camera; };          // atom path -> becomes Main
	struct Sting  { float t = 0; std::string clip; float volume = 1; int bus = 1; };

	std::vector<TransformTrack> transformTracks;
	std::vector<PropTrack>      propTracks;
	std::vector<BoneTrack>      boneTracks;
	std::vector<ClipTrack>      clipTracks;
	std::vector<Event>          events;
	std::vector<Cut>            cuts;
	std::vector<Sting>          stings;

	// Native asset format (.nuseq): JSON — small, diff-able, hand-editable.
	bool             SaveToFile(const std::string& path) const;
	static Sequence* LoadFromFile(const std::string& path);
	static Sequence* LoadFromMemory(const std::string& data);   // packed content
	static Sequence* FromString(const std::string& json);
	std::string      ToString() const;
};

}  // namespace nuke

#endif // !NUKEE_SEQUENCE_H
