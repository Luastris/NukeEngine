#pragma once
#ifndef NUKEE_AUDIO_H
#define NUKEE_AUDIO_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include "Vector.h"
#include "service/iAudio.h"
#include <string>

namespace nuke {

// Static game-side facade over the audio service; null-safe (no-ops / zeros with no provider).
// Voices are addressed by the double id Play() returns (0 = failed); clip paths are
// content-relative. The music-analysis getters read this frame's cached snapshot.
class NUKEENGINE_API Audio
{
	NUKE_CLASS_NOCREATE(Audio, Object)
public:
	[[nuke::func]] static bool Available();   // an audio provider is active

	// One-shot / manual playback. bus: 0 = Music, 1 = SFX, 2 = Preview (editor).
	[[nuke::func]] static double Play(const std::string& clip, double volume, bool loop, double bus);
	[[nuke::func]] static double PlayAt(const std::string& clip, const Vector3& pos,
	                                    double volume, double minDist, double maxDist, double bus);
	// Play encoded audio (ogg/wav/mp3/flac) from memory; the backend copies the bytes.
	// Not [[nuke::func]] — blobs don't fit the reflected value channel; bound per language.
	static double PlayData(const void* bytes, uint64_t size, double volume, bool loop, double bus);
	[[nuke::func]] static void   Stop(double voice);
	[[nuke::func]] static void   StopAll();
	[[nuke::func]] static bool   IsPlaying(double voice);
	[[nuke::func]] static void   SetPaused(double voice, bool paused);
	[[nuke::func]] static void   SetVolume(double voice, double volume);
	[[nuke::func]] static void   SetPitch(double voice, double pitch);
	[[nuke::func]] static void   Seek(double voice, double seconds);
	[[nuke::func]] static double Time(double voice);
	[[nuke::func]] static double Length(double voice);

	// Mix control.
	[[nuke::func]] static void   SetBusVolume(double bus, double volume);
	[[nuke::func]] static double GetBusVolume(double bus);
	[[nuke::func]] static void   SetMasterVolume(double volume);
	[[nuke::func]] static double GetMasterVolume();

	// Music analysis (this frame's snapshot; all [0..1] unless noted).
	[[nuke::func]] static double GetKick();          // drum onset envelope
	[[nuke::func]] static double GetSnare();         // mid/high percussive envelope
	[[nuke::func]] static double GetBass();          // sustained bass energy
	[[nuke::func]] static double GetEnergy();        // overall loudness
	[[nuke::func]] static double GetChroma(double note);   // pitch-class energy, 0=C .. 11=B
	[[nuke::func]] static double GetNote();          // dominant pitch class (0..11)
	[[nuke::func]] static double GetNoteStrength();
	[[nuke::func]] static double GetBeatPhase();     // 0..1 between beats

	// --- engine internals (not reflected) ---
	static const NukeAudioAnalysis& Analysis();      // this frame's cached snapshot
	static void Refresh();                           // pull the snapshot (World::Render, once per frame)
	// Fill a system post-param (g_NukeAudio / g_NukeNote / g_NukeChromaA..C) from the
	// snapshot + engine time. Returns false when `name` is not a system param.
	static bool SystemParam(const std::string& name, float out[4]);
	// Resolve a content-relative clip path against the project content root.
	static std::string ResolveClip(const std::string& clip);
	// Start a voice through the content layers (raw file by path, pak entry from bytes).
	static uint64_t PlayDesc(const std::string& clip, const NukeVoiceDesc& desc);
};

}  // namespace nuke

#endif // !NUKEE_AUDIO_H
