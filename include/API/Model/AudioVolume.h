#pragma once
#ifndef NUKEE_AUDIOVOLUME_H
#define NUKEE_AUDIOVOLUME_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "AudioMixer.h"
#include "reflect/Reflect.h"
#include <string>
#include <vector>

namespace nuke {

class Transform;

// Influence 0..1 of a sphere / box volume at a world point: 1 inside, fading to 0 over the blend
// distance outside the surface; the atom's scale scales the shape (PostFXVolume / ReverbZone rule).
NUKEENGINE_API float VolumeInfluence(Transform* t, int shape, float radius, const Vector3& halfExtents,
                                     float blendDistance, float weight, const Vector3& p);

// An audio volume: while the listener is inside (fading over Blend Distance), (1) its MIXER
// snapshot blends over the live mix - bus volumes, send levels and the parameters of matching
// effects (same bus name, same effect type at the same position of the chain) - and (2) its own
// EFFECTS ride the chosen bus, faded in from their neutral settings (a low-pass from 20 kHz, a
// gain from 0 dB, a compressor from ratio 1, ...) to the authored ones. Overlapping volumes stack
// in ascending priority; outside every volume the project mixer is exact. ReverbZone is the
// special case of one reverb's room.
class NUKEENGINE_API AudioVolume : public Component
{
	NUKE_CLASS(AudioVolume, Component, "Audio")
public:
	[[nuke::prop(label="Shape", enum="Sphere,Box")]]   int     shape = 1;
	[[nuke::prop(label="Radius", min=0)]]              float   radius = 10.0f;                 // sphere
	[[nuke::prop(label="Half Extents")]]               Vector3 halfExtents = Vector3(5, 5, 5);   // box (local)
	[[nuke::prop(label="Priority", tip="Higher wins where volumes overlap (blended in ascending order).")]] int priority = 0;
	[[nuke::prop(label="Blend Distance", min=0, tip="Metres outside the shape over which the influence fades to nothing.")]] float blendDistance = 2.0f;
	[[nuke::prop(label="Weight", min=0, max=1, tip="Full influence inside the shape.")]] float weight = 1.0f;
	[[nuke::prop(asset="file:.numix", label="Mixer", tip="Mixer snapshot: inside, the live mix blends toward this asset's bus volumes, sends and effect parameters (buses matched by name, effects by type and position); empty = none")]] std::string mixer;
	[[nuke::prop(label="Effects Bus", widget="audiobus", tip="The bus this volume's own effects ride on while the listener is inside")]] int bus = 1;
	[[nuke::prop(hidden)]] std::string effectsData;   // the volume's effects (JSON [{type, enabled, params}]); edited via the custom inspector
	std::vector<AudioMixer::Effect> effects;         // parsed from effectsData

	AudioVolume();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override {}
	void Reset() override {}
	void Pause() override {}

	void EnsureParsed();
	void Commit();
	const AudioMixer* Snapshot();                 // the mixer asset, loaded once (null = none / missing)
	float WeightAt(const Vector3& p) const;       // 0 outside the blend band .. weight inside
	static const std::vector<AudioVolume*>& All();
	static void InvalidateSnapshots();            // a .numix changed on disk / the project switched
private:
	std::string parsedFrom;
};

}  // namespace nuke
#endif // !NUKEE_AUDIOVOLUME_H
