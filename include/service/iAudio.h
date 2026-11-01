#pragma once
#ifndef NUKEE_IAUDIO_H
#define NUKEE_IAUDIO_H
#include <cstdint>

namespace nuke {

// Backend-neutral POD voice description crossing the audio seam. A voice plays a plain audio
// file (ogg/wav/mp3/flac) by on-disk path; the backend owns decoding and caching.
struct NukeVoiceDesc
{
	float volume = 1.0f;            // linear gain [0..2]
	float pitch  = 1.0f;            // playback rate multiplier
	bool  loop   = false;

	bool  spatial = false;          // false = plays as-is on both ears (music/UI)
	float pos[3] = { 0, 0, 0 };     // initial WORLD position (spatial only)
	float minDist = 1.0f;           // full volume inside this radius
	float maxDist = 50.0f;          // inaudible beyond this radius
	int   bus = 1;                  // 0 = Music, 1 = SFX, 2 = Preview (editor auditioning, never game-paused)
	int   decode = 0;               // 0 = Auto (by file size), 1 = Memory (decode up front), 2 = Stream
};

// Per-frame analysis of the MASTER mix. Any provider of the "audio" service must fill this
// every update(). Values are smoothed/normalized to [0..1] envelopes unless noted.
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

// ---- D1 mixer PODs (engine abi 63) -------------------------------------------------------
// An effect parameter as the backend describes it (the mixer panel draws from this schema).
struct NukeAudioParamInfo
{
	char  name[24];
	float min = 0.0f, max = 1.0f, def = 0.0f;
	int   unit = 0;   // 0 linear, 1 dB, 2 Hz, 3 ms, 4 ratio
	float neutral = 0.0f;   // the value at which the effect passes the signal unchanged (audio volumes fade effects in from it)
};
struct NukeAudioEffectInfo
{
	char type[16];    // "eq", "compressor", "limiter", "delay", "reverb", "lowpass", "highpass", "gain"
	char label[32];
	int  paramCount = 0;
};
// One effect instance on a bus chain.
struct NukeAudioEffect
{
	char  type[16] = { 0 };
	int   enabled = 1;
	float p[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
};
// One bus of the mixer layout; index = NukeVoiceDesc::bus (0 Music, 1 SFX, 2 Preview by default).
struct NukeAudioBusDesc
{
	char  name[32] = { 0 };
	int   parent = -1;          // another bus's index, -1 = the master
	float volume = 1.0f;
	float reverbSend = 0.0f;    // unused since the generic sends (kept for layout)
};
// One send of a bus: a parallel route into another bus (an aux return is an ordinary bus whose
// chain holds the effect - reverb, delay, ...), at `level` (0..1).
struct NukeAudioSend
{
	int   bus = -1;
	float level = 0.0f;
};

// The audio service contract: the active backend implements it and hands it to the loader via
// NUKEModule::queryService(). Voices are opaque uint64 handles (0 = invalid).
// Threading: called only from the game/render thread; the backend's device callback stays internal.
class iAudio
{
public:
	static constexpr const char* kServiceName = "audio";

	virtual ~iAudio() {}

	// Lifecycle. init is idempotent (first call opens the output device); reset stops all
	// non-preview voices without tearing the device down.
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

	// ABI: new virtuals append at the END, never mid-vtable.
	// Play a clip from memory; the backend copies and owns the bytes for the voice's lifetime.
	// 0 on failure.
	virtual uint64_t playData(const void* bytes, uint64_t size, const NukeVoiceDesc& desc) { return 0; }

	// ---- PCM stream voices ----------------------------------------------------------------
	// Interleaved float32, push from any ONE thread; underrun plays silence (never self-ends).
	// Regular voice calls work on the returned id. 0 = failure.
	virtual uint64_t openStream(int channels, int sampleRate, int bus) { return 0; }
	virtual bool     pushStream(uint64_t voice, const float* interleaved, uint64_t frameCount) { return false; }
	// Frames pushed but not yet consumed by the mixer — pushedTotal minus this, over the
	// sample rate, is the stream's PLAYBACK CLOCK (video sync).
	virtual uint64_t streamQueued(uint64_t voice) { return 0; }
	virtual void     closeStream(uint64_t voice) { stop(voice); }

	// ---- D1 mixer: dynamic buses, per-bus effect chains, metering (engine abi 63) ----------
	// The backend's effect schema: types and their parameters (the mixer panel is built from it).
	virtual int  effectTypeCount() { return 0; }
	virtual bool effectTypeInfo(int index, NukeAudioEffectInfo& out) { return false; }
	virtual bool effectParamInfo(const char* type, int param, NukeAudioParamInfo& out) { return false; }
	// Replace the bus layout. Buses keep their INDEX identity: a voice on bus i plays on through a
	// re-apply; buses past `count` stop their voices. At least 3 buses (bus 2 = Preview by contract).
	virtual bool setMixer(const NukeAudioBusDesc* buses, int count) { return false; }
	virtual int  mixerBusCount() { return 3; }
	// The effect chain of a bus (-1 = the master chain), replaced as a whole.
	virtual bool setBusEffects(int bus, const NukeAudioEffect* fx, int count) { return false; }
	// Superseded by setBusSends / a reverb effect in an aux bus's chain (vtable slots kept; no-ops).
	virtual void setBusReverbSend(int bus, float level) {}
	virtual void setReverb(const NukeAudioEffect& reverb) {}
	// Peak |sample| of the last mixed block: bus >= 0, -1 = master.
	virtual float busLevel(int bus, int channel) { return 0.0f; }
	// ---- D3 occlusion: 0 = clear, 1 = behind geometry (lowpass + attenuation, smoothed) ------
	virtual void setVoiceOcclusion(uint64_t voice, float amount) {}
	// ---- D4 spatial: 0 = the backend's panning spatializer, 1 = binaural (structural HRTF model);
	// applies to voices started after the call.
	virtual void setSpatialMode(int mode) {}
	virtual int  getSpatialMode() { return 0; }
	// ---- Sends (same ABI cycle): a bus's parallel routes into other buses, replaced as a whole.
	// Targets that would loop back (through parents or sends) are dropped by the backend.
	virtual bool setBusSends(int bus, const NukeAudioSend* sends, int count) { return false; }
	// Live parameters of one effect of a bus's chain (-1 = master; index = position in the chain
	// as pushed by setBusEffects) without rebuilding the chain - reverb zones drive rooms with it.
	virtual bool setBusEffectParams(int bus, int index, const float p[8]) { return false; }
};

}  // namespace nuke

#endif // !NUKEE_IAUDIO_H
