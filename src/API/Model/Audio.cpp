// Audio facade — thin, null-safe pass-through to the "audio" service plus the per-frame
// analysis snapshot shared by scripts (nuke.Audio.*) and the g_Nuke* system post-params.
#include "API/Model/Audio.h"
#include "API/Model/Time.h"
#include "interface/Services.h"
#include "interface/AppInstance.h"

namespace nuke {

static NukeAudioAnalysis gSnap;   // this frame's analysis (game thread only)

bool Audio::Available() { return GetService<iAudio>() != nullptr; }

std::string Audio::ResolveClip(const std::string& clip)
{
	if (clip.empty()) return clip;
	return AppInstance::GetSingleton()->ResolveContent(clip);
}

double Audio::Play(const std::string& clip, double volume, bool loop, double bus)
{
	iAudio* a = GetService<iAudio>();
	if (!a || clip.empty()) return 0.0;
	NukeVoiceDesc d;
	d.volume = (float)volume; d.loop = loop; d.bus = (int)bus;
	return (double)a->play(ResolveClip(clip).c_str(), d);
}

double Audio::PlayAt(const std::string& clip, const Vector3& pos,
                     double volume, double minDist, double maxDist, double bus)
{
	iAudio* a = GetService<iAudio>();
	if (!a || clip.empty()) return 0.0;
	NukeVoiceDesc d;
	d.volume = (float)volume; d.bus = (int)bus;
	d.spatial = true;
	d.pos[0] = (float)pos.x; d.pos[1] = (float)pos.y; d.pos[2] = (float)pos.z;
	d.minDist = (float)minDist; d.maxDist = (float)maxDist;
	return (double)a->play(ResolveClip(clip).c_str(), d);
}

void Audio::Stop(double voice)             { if (iAudio* a = GetService<iAudio>()) a->stop((uint64_t)voice); }
void Audio::StopAll()                      { if (iAudio* a = GetService<iAudio>()) a->stopAll(); }
bool Audio::IsPlaying(double voice)        { iAudio* a = GetService<iAudio>(); return a && a->isPlaying((uint64_t)voice); }
void Audio::SetPaused(double voice, bool p){ if (iAudio* a = GetService<iAudio>()) a->setVoicePaused((uint64_t)voice, p); }
void Audio::SetVolume(double voice, double v) { if (iAudio* a = GetService<iAudio>()) a->setVoiceVolume((uint64_t)voice, (float)v); }
void Audio::SetPitch(double voice, double p)  { if (iAudio* a = GetService<iAudio>()) a->setVoicePitch((uint64_t)voice, (float)p); }
void Audio::Seek(double voice, double sec)    { if (iAudio* a = GetService<iAudio>()) a->voiceSeek((uint64_t)voice, (float)sec); }
double Audio::Time(double voice)   { iAudio* a = GetService<iAudio>(); return a ? (double)a->voiceTime((uint64_t)voice) : 0.0; }
double Audio::Length(double voice) { iAudio* a = GetService<iAudio>(); return a ? (double)a->voiceLength((uint64_t)voice) : 0.0; }

void   Audio::SetBusVolume(double bus, double v) { if (iAudio* a = GetService<iAudio>()) a->setBusVolume((int)bus, (float)v); }
double Audio::GetBusVolume(double bus)           { iAudio* a = GetService<iAudio>(); return a ? (double)a->getBusVolume((int)bus) : 1.0; }
void   Audio::SetMasterVolume(double v)          { if (iAudio* a = GetService<iAudio>()) a->setMasterVolume((float)v); }
double Audio::GetMasterVolume()                  { iAudio* a = GetService<iAudio>(); return a ? (double)a->getMasterVolume() : 1.0; }

double Audio::GetKick()         { return gSnap.kick; }
double Audio::GetSnare()        { return gSnap.snare; }
double Audio::GetBass()         { return gSnap.bass; }
double Audio::GetEnergy()       { return gSnap.energy; }
double Audio::GetChroma(double note)
{
	int n = (int)note;
	return (n >= 0 && n < 12) ? gSnap.chroma[n] : 0.0;
}
double Audio::GetNote()         { return (double)gSnap.dominantNote; }
double Audio::GetNoteStrength() { return gSnap.noteStrength; }
double Audio::GetBeatPhase()    { return gSnap.beatPhase; }

const NukeAudioAnalysis& Audio::Analysis() { return gSnap; }

void Audio::Refresh()
{
	if (iAudio* a = GetService<iAudio>()) a->getAnalysis(gSnap);
	else gSnap = NukeAudioAnalysis{};   // provider unloaded mid-session -> calm zeros
}

// System post-params — the ONE naming convention that makes any post shader
// audio-reactive: declare these in its PostParams cbuffer (no initializers) and the
// engine fills them per frame while packing the effect blob (World::Render).
bool Audio::SystemParam(const std::string& name, float out[4])
{
	if (name == "g_NukeAudio")
	{
		out[0] = gSnap.kick; out[1] = gSnap.snare; out[2] = gSnap.bass; out[3] = gSnap.energy;
		return true;
	}
	if (name == "g_NukeNote")
	{
		out[0] = (float)gSnap.dominantNote; out[1] = gSnap.noteStrength;
		out[2] = gSnap.beatPhase; out[3] = (float)Time::Elapsed();
		return true;
	}
	if (name == "g_NukeChromaA") { for (int i = 0; i < 4; ++i) out[i] = gSnap.chroma[i];     return true; }
	if (name == "g_NukeChromaB") { for (int i = 0; i < 4; ++i) out[i] = gSnap.chroma[4 + i]; return true; }
	if (name == "g_NukeChromaC") { for (int i = 0; i < 4; ++i) out[i] = gSnap.chroma[8 + i]; return true; }
	return false;
}

}  // namespace nuke
