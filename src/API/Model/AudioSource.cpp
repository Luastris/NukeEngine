// AudioSource — component driver over the audio service. Voice lifetime follows PLAY
// mode: playOnStart fires on the first playing Update (both PIE and Player), Pause()
// halts the voice (per-voice; the global game pause in World::Render also covers it),
// Destroy/Reset stop it (PIE Stop restores the snapshot, which destroys the component).
#include "API/Model/AudioSource.h"
#include "API/Model/Audio.h"
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "interface/Services.h"

namespace nuke {

AudioSource::AudioSource() : Component("AudioSource") {}

void AudioSource::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void AudioSource::Play()
{
	iAudio* a = GetService<iAudio>();
	if (!a || clip.empty()) return;
	if (voice) { a->stop(voice); voice = 0; }
	NukeVoiceDesc d;
	d.volume = volume; d.pitch = pitch; d.loop = loop;
	d.bus = (bus == 0) ? 0 : 1;
	d.decode = decode;
	d.spatial = spatial;
	if (spatial && transform)
	{
		Vector3 p = transform->globalPosition();
		d.pos[0] = (float)p.x; d.pos[1] = (float)p.y; d.pos[2] = (float)p.z;
		d.minDist = minDist; d.maxDist = maxDist;
	}
	voice = a->play(Audio::ResolveClip(clip).c_str(), d);
	lastVolume = volume; lastPitch = pitch;
}

void AudioSource::Stop()
{
	if (voice)
		if (iAudio* a = GetService<iAudio>()) a->stop(voice);
	voice = 0;
}

void AudioSource::SetPaused(bool paused)
{
	if (voice)
		if (iAudio* a = GetService<iAudio>()) a->setVoicePaused(voice, paused);
}

bool AudioSource::IsPlaying()
{
	iAudio* a = GetService<iAudio>();
	return voice && a && a->isPlaying(voice);
}

void AudioSource::Update()
{
	// World::Update only runs in PLAY mode (PIE / Player), so this IS the play-mode hook.
	if (!enabled) { Stop(); return; }
	if (playOnStart && !started) { started = true; Play(); }

	iAudio* a = GetService<iAudio>();
	if (!a || !voice) return;
	if (!a->isPlaying(voice)) { voice = 0; return; }   // finished (non-loop) — release the handle
	if (spatial && transform)
	{
		Vector3 p = transform->globalPosition();
		float pos[3] = { (float)p.x, (float)p.y, (float)p.z };
		a->setVoicePos(voice, pos);
	}
	// Live inspector edits reach the playing voice.
	if (volume != lastVolume) { a->setVoiceVolume(voice, volume); lastVolume = volume; }
	if (pitch  != lastPitch)  { a->setVoicePitch(voice, pitch);  lastPitch  = pitch; }
}

void AudioSource::FixedUpdate() {}
void AudioSource::Pause()   { SetPaused(true); }
void AudioSource::Destroy() { Stop(); }
void AudioSource::Reset()   { Stop(); started = false; }

}  // namespace nuke
