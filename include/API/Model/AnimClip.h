#pragma once
#ifndef NUKEE_ANIMCLIP_H
#define NUKEE_ANIMCLIP_H
#include "NukeAPI.h"
#include <istream>
#include <string>
#include <vector>

namespace nuke {

// One animation clip asset (.nuanim, roadmap 3.1). Channels address skeleton joints BY
// NAME (retarget-friendly: any mesh whose bones carry the same names can play the clip);
// key times are SECONDS (import already divides by the source ticks-per-second).
// Sampling/blending lives in the Animator component — the clip is pure data.
class NUKEENGINE_API AnimClip
{
public:
	std::string guid;    // asset id (ResDB)
	std::string name;    // clip name (shown in pickers / addressed by Animator states)
	double duration = 0.0;   // seconds

	struct Key { float t; float v[4]; };   // v = xyz (pos/scale, w unused) or xyzw (rotation quat)
	struct Channel
	{
		std::string bone;                  // joint name
		std::vector<Key> pos, rot, scl;    // any may be empty (that TRS part stays at bind)
	};
	std::vector<Channel> channels;

	// Named time markers (v2): the Animator fires Component::OnAnimEvent on its atom when
	// the playhead crosses one (loop-aware). Kept sorted by t (AddEvent inserts in order).
	struct Event { float t; std::string name; };
	std::vector<Event> events;
	void AddEvent(float t, const std::string& name);

	// Native asset format (.nuanim): binary, same header style as .numesh.
	bool             SaveToFile(const std::string& path) const;
	static AnimClip* LoadFromFile(const std::string& path);
	static AnimClip* LoadFromMemory(const std::string& data);   // packed content (3.2)
	static AnimClip* LoadFromStream(std::istream& i);
};

}  // namespace nuke

#endif // !NUKEE_ANIMCLIP_H
