#pragma once
#ifndef NUKEE_ANIMCLIP_H
#define NUKEE_ANIMCLIP_H
#include "NukeAPI.h"
#include <istream>
#include <string>
#include <vector>
#include "reflect/Reflect.h"   // NUKE_CLASS (reflected asset)

namespace nuke {

// One animation clip asset (.nuanim): pure data. Channels address joints BY NAME (retarget-
// friendly); key times are in SECONDS. Sampling/blending lives in the Animator component.
class NUKEENGINE_API AnimClip
{
	NUKE_CLASS(AnimClip, Object)
public:
	std::string guid;    // asset id (ResDB)
	[[nuke::prop(label="Name")]]     std::string name;    // clip name (shown in pickers / addressed by Animator states)
	[[nuke::prop(label="Duration")]] double duration = 0.0;   // seconds

	struct Key { float t; float v[4]; };   // v = xyz (pos/scale, w unused) or xyzw (rotation quat)
	struct Channel
	{
		std::string bone;                  // joint name
		std::vector<Key> pos, rot, scl;    // any may be empty (that TRS part stays at bind)
	};
	std::vector<Channel> channels;

	// Named time markers; the Animator fires Component::OnAnimEvent when the playhead
	// crosses one. Kept sorted by t (AddEvent inserts in order).
	struct Event { float t; std::string name; };
	std::vector<Event> events;
	[[nuke::func]] void AddEvent(float t, const std::string& name);

	// Native asset format (.nuanim): binary, same header style as .numesh.
	bool             SaveToFile(const std::string& path) const;
	static AnimClip* LoadFromFile(const std::string& path);
	static AnimClip* LoadFromMemory(const std::string& data);   // packed content (3.2)
	static AnimClip* LoadFromStream(std::istream& i);
};

}  // namespace nuke

#endif // !NUKEE_ANIMCLIP_H
