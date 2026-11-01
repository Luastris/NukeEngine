#pragma once
#ifndef NUKEE_AUDIOMIXER_H
#define NUKEE_AUDIOMIXER_H
#include "NukeAPI.h"
#include "service/iAudio.h"
#include <string>
#include <vector>

namespace nuke {

// The project's mixer: the bus layout, each bus's effect chain and sends, the master chain and
// the spatial mode. A `.numix` asset (JSON) in content, named by the project (`audioMixer` in
// the .nuproj; none = the built-in defaults); the editor's Audio Mixer panel edits it live. Bus
// index = NukeVoiceDesc::bus; the first three are Music / SFX / Preview (bus 2 = the editor's
// preview bus by contract). Effects are inserts in a bus's chain (the default puts a light reverb
// on SFX); a bus may also SEND into any other bus, so an aux return is just a bus with the effect
// in its chain - nothing is built in.
class NUKEENGINE_API AudioMixer
{
public:
	struct Effect { std::string type; bool enabled = true; float p[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }; };
	struct Send   { std::string bus; float level = 0.0f; };   // target bus by name (indices shift), 0..1
	struct Bus
	{
		std::string name;
		int   parent = -1;          // another bus's index, -1 = master
		float volume = 1.0f;
		std::vector<Effect> effects;
		std::vector<Send>   sends;
	};
	std::vector<Bus>    buses;
	std::vector<Effect> master;      // the master chain (a limiter by default)
	float   masterVolume = 1.0f;
	int     spatialMode = 0;         // 0 = panning, 1 = binaural

	static AudioMixer Default();     // Music / SFX (a light reverb insert) / Preview, master limiter
	static Effect     DefaultEffect(iAudio* a, const std::string& type);   // the backend's defaults for a type

	bool        LoadFromString(const std::string& json);
	std::string SaveToString() const;
	bool        LoadFromFile(const std::string& fullPath);
	bool        SaveToFile(const std::string& fullPath) const;
	int         BusIndex(const std::string& name) const;   // -1 = none
	int         EffectIndex(int bus, const std::string& type) const;   // first effect of that type in a bus's chain (-1 = master), -1 = none
	void        Apply(iAudio* a) const;                     // push the whole mixer to the backend
	void        ApplySends(iAudio* a, int bus) const;       // one bus's sends (names resolved to indices)

	// The live mixer of the session: loaded from AppInstance::audioMixerPath on first use (the
	// default when the file is missing), applied to the backend once per audio init.
	static AudioMixer&        Current();
	static const std::string& CurrentPath();                // content-relative source ("" = defaults)
	static bool Load(const std::string& contentRel);        // load + apply (the facade / panel)
	static void Set(const AudioMixer& m, bool apply);       // replace + optionally apply (the panel's live edits)
	static void EnsureApplied(iAudio* a);                   // World::Render: first load, re-apply after a backend init
	static void Invalidate();                               // the file changed on disk / project switched: reload on next use
	static unsigned ApplyCount();                           // bumps on every whole-mixer push (the live-mix driver re-syncs its cache)
};

}  // namespace nuke
#endif // !NUKEE_AUDIOMIXER_H
