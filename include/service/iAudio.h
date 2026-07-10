#pragma once
#ifndef NUKEE_IAUDIO_H
#define NUKEE_IAUDIO_H
#include <cstdint>

namespace nuke {

// Backend-neutral voice description crossing the audio seam. POD only — no backend
// (miniaudio/FMOD/...) and no engine model types, exactly like the iRender/iPhysics seams.
// Clips have NO custom asset format: a voice plays a plain audio FILE (ogg/wav/mp3/flac)
// referenced by its full on-disk path; the backend owns decoding and caching.
struct NukeVoiceDesc
{
	float volume = 1.0f;            // linear gain [0..2]
	float pitch  = 1.0f;            // playback rate multiplier
	bool  loop   = false;

	// 3D spatialization. Non-spatial voices (music/UI) play as-is on both ears.
	bool  spatial = false;
	float pos[3] = { 0, 0, 0 };     // initial WORLD position (spatial only)
	float minDist = 1.0f;           // full volume inside this radius
	float maxDist = 50.0f;          // inaudible beyond this radius

	// Mix bus: 0 = Music, 1 = SFX, 2 = Preview (editor-only auditioning; never game-paused).
	int   bus = 1;

	// Decode mode: 0 = Auto (backend picks by file size), 1 = Memory (decode fully up
	// front — short SFX), 2 = Stream (decode on the fly — long music tracks).
	int   decode = 0;
};

// Per-frame music analysis of the MASTER mix, produced by the backend's DSP tap. This is
// the compatibility contract between the audio module and audio-reactive consumers (the
// musicvis post-effect, scripts): whoever provides "audio" must fill it every update().
// All values are smoothed/normalized to [0..1] envelopes unless noted.
struct NukeAudioAnalysis
{
	float kick   = 0.0f;        // percussive low-band onset envelope (kick / low drums)
	float snare  = 0.0f;        // percussive mid/high-band onset envelope (snare / hats)
	float bass   = 0.0f;        // sustained low-frequency energy (bass guitar / synth bass)
	float energy = 0.0f;        // overall loudness (RMS)

	float chroma[12] = { 0 };   // per-pitch-class energy, C=0 .. B=11, normalized to peak
	int   dominantNote = 0;     // argmax(chroma)
	float noteStrength = 0.0f;  // how dominant that note is [0..1]

	float beatPhase = 0.0f;     // 0..1 progress from the last detected beat to the expected next
	float bpm       = 0.0f;     // rough tempo estimate (0 = unknown yet)
};

// The AUDIO service contract (unified plugin model). The active audio backend (NukeAudio
// today) implements this and hands it to the loader via NUKEModule::queryService(); the
// ENGINE drives it once per frame from World::Render (both hosts render every frame) —
// the backend only mixes and analyses. Voices are opaque uint64 handles (0 = invalid).
//
// Threading: everything is called from the game/render thread; the backend's device
// callback runs on its own audio thread INTERNALLY and must not surface here.
class iAudio
{
public:
	static constexpr const char* kServiceName = "audio";

	virtual ~iAudio() {}

	// Lifecycle. init is idempotent (first call opens the output device); reset stops all
	// non-preview voices (play stop / world switch) without tearing the device down.
	virtual bool init() = 0;
	virtual void reset() = 0;

	// Per-frame pump: advances fades, refreshes the analysis snapshot. dt in seconds.
	virtual void update(float dt) = 0;

	// Listener pose for spatial voices (world position, forward, up).
	virtual void setListener(const float pos[3], const float fwd[3], const float up[3]) = 0;

	// Voice lifecycle. `path` is a full on-disk file path (ogg/wav/mp3/flac). 0 on failure.
	virtual uint64_t play(const char* path, const NukeVoiceDesc& desc) = 0;
	virtual void     stop(uint64_t voice) = 0;
	virtual void     stopAll() = 0;                       // every bus, preview included

	virtual void setVoicePaused(uint64_t voice, bool paused) = 0;
	virtual bool isPlaying(uint64_t voice) = 0;           // false once finished (non-loop) or stopped
	virtual void setVoiceVolume(uint64_t voice, float volume) = 0;
	virtual void setVoicePitch(uint64_t voice, float pitch) = 0;
	virtual void setVoicePos(uint64_t voice, const float pos[3]) = 0;

	// Time within the voice's clip, in seconds (UI/scrubbing). length 0 when unknown.
	virtual float voiceTime(uint64_t voice) = 0;
	virtual float voiceLength(uint64_t voice) = 0;
	virtual void  voiceSeek(uint64_t voice, float seconds) = 0;

	// Mix control. Bus indices match NukeVoiceDesc::bus; master scales everything.
	virtual void  setBusVolume(int bus, float volume) = 0;
	virtual float getBusVolume(int bus) = 0;
	virtual void  setMasterVolume(float volume) = 0;
	virtual float getMasterVolume() = 0;

	// Game pause: pauses Music + SFX voices (NOT Preview), resumes them on false.
	virtual void setGamePaused(bool paused) = 0;

	// Latest master-mix analysis (refreshed in update()).
	virtual void getAnalysis(NukeAudioAnalysis& out) = 0;

	// ABI: new virtuals append at the END, never mid-vtable (see iRender's incident note).
	// Play a clip from MEMORY (packed content, 3.2): the backend COPIES/owns the bytes for
	// the voice's lifetime (decode + streaming both run off its own copy). 0 on failure.
	virtual uint64_t playData(const void* bytes, uint64_t size, const NukeVoiceDesc& desc) { return 0; }
};

}  // namespace nuke

#endif // !NUKEE_IAUDIO_H
