// AudioSource — component driver over the audio service; voice lifetime follows play mode.
#include "API/Model/AudioSource.h"
#include "API/Model/Audio.h"
#include "API/Model/Time.h"
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "API/Model/Physics.h"
#include "API/Model/Game.h"          // Game::GetTimeScale
#include "interface/AppInstance.h"   // playState: the scale applies while playing
#include "interface/Services.h"
#include <cmath>

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
	started = true;   // a scripted Play before the first Update counts as the start: no second voice from Play On Start
	if (voice) { a->stop(voice); voice = 0; }
	NukeVoiceDesc d;
	d.volume = volume; d.pitch = pitch; d.loop = loop;
	d.bus = bus;
	d.decode = decode;
	d.spatial = spatial;
	if (spatial && transform)
	{
		Vector3 p = transform->globalPosition();
		d.pos[0] = (float)p.x; d.pos[1] = (float)p.y; d.pos[2] = (float)p.z;
		d.minDist = minDist; d.maxDist = maxDist;
	}
	voice = Audio::PlayDesc(clip, d);   // raw file by path; pak entry from memory
	lastVolume = volume; lastPitch = pitch;
	// Started under a stopped / scaled clock: the voice takes the clock's state at once.
	const double ts = ClockScale();
	timeFrozen = ts <= 1e-4;
	if (!timeFrozen && ts != 1.0) { lastPitch = pitch * (float)ts; a->setVoicePitch(voice, lastPitch); }
	PushPause(a);
}

void AudioSource::Stop()
{
	if (voice)
		if (iAudio* a = GetService<iAudio>()) a->stop(voice);
	voice = 0;
	timeFrozen = false;
}

void AudioSource::SetPaused(bool paused)
{
	userPaused = paused;
	if (voice)
		if (iAudio* a = GetService<iAudio>()) PushPause(a);
}

double AudioSource::ClockScale()
{
	// Game.TimeScale drives the game clock only while PLAYING (Time::NewFrame: edit mode = real time).
	const double gs = (AppInstance::GetSingleton()->playState == 1) ? Game::GetTimeScale() : 1.0;
	return gs * Time::LocalScale();
}

void AudioSource::PushPause(iAudio* a)
{
	if (a && voice) a->setVoicePaused(voice, userPaused || timeFrozen);
}

bool AudioSource::IsPlaying()
{
	iAudio* a = GetService<iAudio>();
	return voice && a && a->isPlaying(voice);
}

double AudioSource::Occlusion() { return occl; }

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
		// D3 occlusion: a ray from the listener toward the source, one source in four per frame
		// (staggered by id), the result smoothed so a doorway crossing is a fade, not a click.
		if (occlusion && Physics::Available())
		{
			static unsigned frameStagger = 0;
			if (((unsigned)id.id + (frameStagger++ >> 2)) % 4 == 0)
			{
				const Vector3 L = Audio::ListenerPos();
				const Vector3 dir(p.x - L.x, p.y - L.y, p.z - L.z);
				const double dist = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
				occlTarget = (dist > 0.3 && Physics::RaycastIgnore(L, dir, dist - 0.15, atom)) ? 1.0f : 0.0f;
			}
		}
		else occlTarget = 0.0f;
		const float dt = (float)Time::Delta();
		occl += (occlTarget - occl) * (dt * 8.0f > 1.0f ? 1.0f : dt * 8.0f);
		if (std::fabs(occl - occlSent) > 0.002f) { a->setVoiceOcclusion(voice, occl); occlSent = occl; }
	}
	// Live inspector edits reach the playing voice.
	if (volume != lastVolume) { a->setVoiceVolume(voice, volume); lastVolume = volume; }
	// The game clock: Game.TimeScale x the TimeVolume's local scale bends the pitch; 0 = paused.
	const double ts = ClockScale();
	const bool frozen = ts <= 1e-4;
	if (frozen != timeFrozen) { timeFrozen = frozen; PushPause(a); }
	if (!frozen)
	{
		const float effPitch = pitch * (float)ts;
		if (effPitch != lastPitch) { a->setVoicePitch(voice, effPitch); lastPitch = effPitch; }
	}
}

void AudioSource::FixedUpdate() {}
void AudioSource::Pause()   { SetPaused(true); }
void AudioSource::Destroy() { Stop(); }
void AudioSource::Reset()   { Stop(); started = false; userPaused = false; }

}  // namespace nuke
