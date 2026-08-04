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
	// Skeleton the clip was authored on (v4; "" = unknown/legacy). A mismatch against the
	// playing skeleton triggers the cached chain retarget (Retarget.h).
	std::string skelGuid;

	struct Key { float t; float v[4]; };   // v = xyz (pos/scale, w unused) or xyzw (rotation quat)
	// Piecewise-linear track sampling (clamped past the ends); Quat slerps and normalizes.
	static void Sample(const std::vector<Key>& keys, double t, float out[4]);
	static void SampleQuat(const std::vector<Key>& keys, double t, float out[4]);
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

	// Typed notify (v3): engine-side action fired when the playhead crosses t. Every type ALSO
	// broadcasts OnAnimEvent(name) to sibling components/scripts.
	//   type 0 Event       — name only (the broadcast is the whole action)
	//   type 1 SpawnPrefab — asset = content-relative .nuprefab path, socket = anchor
	//                        ("" = the animated atom), a = lifetime seconds (0 = keep)
	//   type 2 Sound       — asset = audio clip guid, socket = 3D anchor ("" = 2D), a = volume
	//   type 3 Shake       — main game camera shake: a = amplitude, b = frequency Hz, c = seconds
	struct Notify
	{
		float t = 0;
		int   type = 0;
		std::string name;
		std::string asset;
		std::string socket;
		float a = 0, b = 0, c = 0;
	};
	std::vector<Notify> notifies;
	[[nuke::func]] void AddNotify(double t, double type, const std::string& name,
	                              const std::string& asset, const std::string& socket,
	                              double a, double b, double c);

	// Named float curves (v3): sampled with the clip, blended by the Animator, read via
	// Animator::CurveValue(name). Keys use v[0].
	struct Curve { std::string name; std::vector<Key> keys; };
	std::vector<Curve> curves;
	[[nuke::func]] void AddCurveKey(const std::string& curve, double t, double v);

	// Property tracks (v3): the clip animates ANY reflected component prop next to the skeleton —
	// muzzle-flash light intensity, emissive strength... `path` walks atom NAMES down from the
	// Animator's atom ("" = the atom itself, "Muzzle/Flash" descends); `comp` is the component
	// type, `prop` the reflected field. dim = used key components (1..4; bools read v[0] >= 0.5
	// and STEP between keys).
	struct PropTrack
	{
		std::string path, comp, prop;
		int dim = 1;
		std::vector<Key> keys;
	};
	std::vector<PropTrack> propTracks;
	[[nuke::func]] void AddPropKey(const std::string& path, const std::string& comp,
	                               const std::string& prop, double t, double v);

	// Native asset format (.nuanim): binary, same header style as .numesh.
	bool             SaveToFile(const std::string& path) const;
	static AnimClip* LoadFromFile(const std::string& path);
	static AnimClip* LoadFromMemory(const std::string& data);   // packed content (3.2)
	static AnimClip* LoadFromStream(std::istream& i);
};

}  // namespace nuke

#endif // !NUKEE_ANIMCLIP_H
