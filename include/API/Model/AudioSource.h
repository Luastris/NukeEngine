#pragma once
#ifndef NUKEE_AUDIOSOURCE_H
#define NUKEE_AUDIOSOURCE_H
#include "NukeAPI.h"
#include "Component.h"
#include "reflect/Reflect.h"
#include <cstdint>
#include <string>

namespace nuke {

// Plays an audio file, optionally at the atom's position. Thin driver over the audio service;
// the clip is a plain ogg/wav/mp3/flac in project content, by content-relative path.
// Playback runs in PLAY mode — the editor previews clips through the Preview bus instead.
class NUKEENGINE_API AudioSource : public Component
{
	NUKE_CLASS(AudioSource, Component, "Audio")
public:
	[[nuke::prop(asset="audio", label="Clip")]]        std::string clip;        // content-relative path
	enum Bus : int { Music = 0, SFX = 1 };
	[[nuke::prop(label="Bus", enum="Music,SFX")]]      Bus   bus = SFX;
	[[nuke::prop(label="Volume", min=0, max=2)]]       float volume = 1.0f;
	[[nuke::prop(label="Pitch",  min=0.1, max=3)]]     float pitch = 1.0f;
	[[nuke::prop(label="Loop")]]                       bool  loop = false;
	[[nuke::prop(label="Play On Start")]]              bool  playOnStart = true;
	[[nuke::prop(label="Spatial (3D)")]]               bool  spatial = false;
	[[nuke::prop(label="Min Distance", min=0.01)]]     float minDist = 1.0f;
	[[nuke::prop(label="Max Distance", min=0.1)]]      float maxDist = 50.0f;
	enum DecodeMode : int { Auto = 0, Memory = 1, Stream = 2 };
	[[nuke::prop(label="Decode", enum="Auto,Memory,Stream")]] DecodeMode decode = Auto;

	// Script/game control (auto-bound via component reflection).
	[[nuke::func]] void Play();
	[[nuke::func]] void Stop();
	[[nuke::func]] void SetPaused(bool paused);
	[[nuke::func]] bool IsPlaying();

	uint64_t voice = 0;   // live voice handle (0 = none); runtime only, not serialized

	AudioSource();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

private:
	bool started = false;        // playOnStart consumed (rearmed on Reset/stop)
	float lastVolume = -1.0f, lastPitch = -1.0f;   // push live inspector edits to the voice
};

}  // namespace nuke

#endif // !NUKEE_AUDIOSOURCE_H
