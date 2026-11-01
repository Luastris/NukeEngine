// Audio facade — null-safe pass-through to the "audio" service + the per-frame analysis snapshot.
#include "API/Model/Audio.h"
#include "API/Model/AudioMixer.h"
#include "API/Model/Time.h"
#include "interface/Services.h"
#include "interface/AppInstance.h"
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cctype>

namespace nuke {

static NukeAudioAnalysis gSnap;   // this frame's analysis (game thread only)
static Vector3 gLisPos(0, 0, 0), gLisFwd(0, 0, 1), gLisUp(0, 1, 0);

bool        Audio::LoadMixer(const std::string& contentRel) { return AudioMixer::Load(contentRel); }
std::string Audio::MixerPath()                              { return AudioMixer::CurrentPath(); }
double      Audio::Bus(const std::string& name)             { return (double)AudioMixer::Current().BusIndex(name); }
double      Audio::BusCount()                               { return (double)AudioMixer::Current().buses.size(); }
std::string Audio::BusName(double bus)
{
	const AudioMixer& m = AudioMixer::Current();
	const int b = (int)bus;
	return (b >= 0 && b < (int)m.buses.size()) ? m.buses[b].name : std::string();
}
double Audio::BusLevel(double bus, double channel) { iAudio* a = GetService<iAudio>(); return a ? (double)a->busLevel((int)bus, (int)channel) : 0.0; }
void Audio::SetBusSend(double bus, double target, double level)
{
	AudioMixer& m = AudioMixer::Current();
	const int b = (int)bus, t = (int)target;
	if (b < 0 || b >= (int)m.buses.size() || t < 0 || t >= (int)m.buses.size() || t == b) return;
	std::vector<AudioMixer::Send>& ss = m.buses[b].sends;
	const std::string name = m.buses[t].name;
	auto it = std::find_if(ss.begin(), ss.end(), [&](const AudioMixer::Send& s) { return s.bus == name; });
	if (level <= 0.0) { if (it != ss.end()) ss.erase(it); }
	else if (it != ss.end()) it->level = (float)level;
	else ss.push_back({ name, (float)level });
	if (iAudio* a = GetService<iAudio>()) m.ApplySends(a, b);
}
double Audio::GetBusSend(double bus, double target)
{
	const AudioMixer& m = AudioMixer::Current();
	const int b = (int)bus, t = (int)target;
	if (b < 0 || b >= (int)m.buses.size() || t < 0 || t >= (int)m.buses.size()) return 0.0;
	for (const AudioMixer::Send& s : m.buses[b].sends) if (s.bus == m.buses[t].name) return s.level;
	return 0.0;
}
void   Audio::SetSpatialMode(double mode)
{
	AudioMixer::Current().spatialMode = (int)mode == 1 ? 1 : 0;
	if (iAudio* a = GetService<iAudio>()) a->setSpatialMode((int)mode);
}
double Audio::GetSpatialMode()  { iAudio* a = GetService<iAudio>(); return a ? (double)a->getSpatialMode() : (double)AudioMixer::Current().spatialMode; }
void   Audio::SetListenerPose(const Vector3& pos, const Vector3& fwd, const Vector3& up) { gLisPos = pos; gLisFwd = fwd; gLisUp = up; }
const Vector3& Audio::ListenerPos() { return gLisPos; }

bool Audio::Available() { return GetService<iAudio>() != nullptr; }

std::string Audio::ResolveClip(const std::string& clip)
{
	if (clip.empty()) return clip;
	return AppInstance::GetSingleton()->ResolveContent(clip);
}

// Start a voice: a real file plays by path (streamed); a pak-only clip is decoded from memory.
// A missing path retries sibling audio extensions — packaging ships transcoded clips under
// their own extension (beep.wav -> beep.ogg) while references keep the authored name.
uint64_t Audio::PlayDesc(const std::string& clip, const NukeVoiceDesc& d)
{
	iAudio* a = GetService<iAudio>();
	if (!a || clip.empty()) return 0;
	// Init on demand (idempotent): the first World::Update runs before the first Render pump.
	if (!a->init()) return 0;
	auto tryOne = [&](const std::string& rel) -> uint64_t
	{
		std::string full = ResolveClip(rel);
		boost::system::error_code ec;
		if (!full.empty() && boost::filesystem::exists(boost::filesystem::path(full), ec))
			return a->play(full.c_str(), d);
		std::string bytes;
		if (AppInstance::GetSingleton()->ReadContent(rel, bytes) && !bytes.empty())
			return a->playData(bytes.data(), bytes.size(), d);
		return 0;
	};
	if (uint64_t v = tryOne(clip)) return v;
	const size_t dot = clip.find_last_of('.');
	if (dot == std::string::npos) return 0;
	std::string ext = clip.substr(dot);
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	static const char* kExts[] = { ".ogg", ".wav", ".flac", ".mp3" };
	bool audio = false;
	for (const char* e : kExts) audio = audio || ext == e;
	if (!audio) return 0;
	for (const char* e : kExts)
		if (ext != e)
			if (uint64_t v = tryOne(clip.substr(0, dot) + e)) return v;
	return 0;
}

double Audio::Play(const std::string& clip, double volume, bool loop, double bus)
{
	NukeVoiceDesc d;
	d.volume = (float)volume; d.loop = loop; d.bus = (int)bus;
	return (double)PlayDesc(clip, d);
}

double Audio::PlayData(const void* bytes, uint64_t size, double volume, bool loop, double bus)
{
	iAudio* a = GetService<iAudio>();
	if (!a || !bytes || !size) return 0.0;
	if (!a->init()) return 0.0;   // same on-demand init as PlayDesc
	NukeVoiceDesc d;
	d.volume = (float)volume; d.loop = loop; d.bus = (int)bus;
	return (double)a->playData(bytes, size, d);
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
	return (double)PlayDesc(clip, d);
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

// System post-params: names a post shader declares in its PostParams cbuffer (no
// initializers) that the engine fills per frame. Returns false for an unknown name.
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
