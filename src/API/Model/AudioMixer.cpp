// AudioMixer — the project's bus / effect layout as a JSON asset, pushed through the audio seam.
#include "API/Model/AudioMixer.h"
#include "interface/AppInstance.h"
#include "interface/Services.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace nuke {

using json = nlohmann::json;

static AudioMixer  gCurrent;
static std::string gCurrentPath;
static bool        gLoaded = false, gApplied = false;
static const void* gAppliedTo = nullptr;   // the backend the live mixer was pushed to

AudioMixer::Effect AudioMixer::DefaultEffect(iAudio* a, const std::string& type)
{
	Effect e; e.type = type;
	if (!a) return e;
	NukeAudioParamInfo pi;
	for (int i = 0; i < 8 && a->effectParamInfo(type.c_str(), i, pi); ++i) e.p[i] = pi.def;
	return e;
}

AudioMixer AudioMixer::Default()
{
	AudioMixer m;
	Bus music; music.name = "Music";
	Bus sfx;   sfx.name = "SFX";
	Effect r; r.type = "reverb";   // a light room as an INSERT on SFX (wet 0.2 over the dry signal); ReverbZones drive it
	r.p[0] = 0.7f; r.p[1] = 0.5f; r.p[2] = 0.2f; r.p[3] = 1.0f; r.p[4] = 1.0f; r.p[5] = 10.0f;
	sfx.effects = { r };
	Bus prev;  prev.name = "Preview";
	m.buses = { music, sfx, prev };
	Effect lim; lim.type = "limiter"; lim.p[0] = -1.0f; lim.p[1] = 50.0f;
	m.master = { lim };
	return m;
}

static json EffectToJson(const AudioMixer::Effect& e)
{
	json j; j["type"] = e.type; j["enabled"] = e.enabled;
	json p = json::array(); for (float v : e.p) p.push_back(v); j["params"] = p;
	return j;
}
static AudioMixer::Effect EffectFromJson(const json& j)
{
	AudioMixer::Effect e;
	e.type = j.value("type", std::string());
	e.enabled = j.value("enabled", true);
	if (j.contains("params") && j["params"].is_array())
	{
		int i = 0;
		for (const json& v : j["params"]) { if (i >= 8) break; e.p[i++] = v.is_number() ? v.get<float>() : 0.0f; }
	}
	return e;
}

bool AudioMixer::LoadFromString(const std::string& text)
{
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return false;
	AudioMixer m;
	std::vector<float> v1Sends;   // version 1: per-bus "reverbSend" into a built-in aux
	if (j.contains("buses") && j["buses"].is_array())
		for (const json& bj : j["buses"])
		{
			Bus b;
			b.name = bj.value("name", std::string());
			b.parent = bj.value("parent", -1);
			b.volume = bj.value("volume", 1.0f);
			if (bj.contains("effects") && bj["effects"].is_array()) for (const json& ej : bj["effects"]) b.effects.push_back(EffectFromJson(ej));
			if (bj.contains("sends") && bj["sends"].is_array())
				for (const json& sj : bj["sends"])
					if (sj.is_object()) b.sends.push_back({ sj.value("bus", std::string()), sj.value("level", 0.0f) });
			v1Sends.push_back(bj.value("reverbSend", 0.0f));
			m.buses.push_back(b);
		}
	if (j.contains("master") && j["master"].is_array()) for (const json& ej : j["master"]) m.master.push_back(EffectFromJson(ej));
	m.masterVolume = j.value("masterVolume", 1.0f);
	m.spatialMode = j.value("spatialMode", 0);
	// The contract: at least Music / SFX / Preview.
	const AudioMixer d = Default();
	for (size_t i = m.buses.size(); i < 3; ++i) { m.buses.push_back(d.buses[i]); v1Sends.push_back(0.0f); }
	// Version 1 files (a built-in aux reverb fed by per-bus reverbSend): every bus that sent into
	// it gets the reverb as an insert at the end of its chain, wet scaled by its send, dry 1.
	if (j.value("version", 1) < 2)
	{
		Effect r = (j.contains("reverb") && j["reverb"].is_object()) ? EffectFromJson(j["reverb"]) : d.buses[1].effects[0];
		r.type = "reverb";
		for (size_t i = 0; i < m.buses.size() && i < v1Sends.size(); ++i)
		{
			if (v1Sends[i] <= 0.0f) continue;
			Effect e = r; e.p[2] = r.p[2] * v1Sends[i]; e.p[3] = 1.0f;
			m.buses[i].effects.push_back(e);
		}
	}
	*this = m;
	return true;
}

std::string AudioMixer::SaveToString() const
{
	json j;
	j["type"] = "AudioMixer"; j["version"] = 2;
	json bs = json::array();
	for (const Bus& b : buses)
	{
		json bj; bj["name"] = b.name; bj["parent"] = b.parent; bj["volume"] = b.volume;
		json es = json::array(); for (const Effect& e : b.effects) es.push_back(EffectToJson(e)); bj["effects"] = es;
		json ss = json::array(); for (const Send& s : b.sends) { json sj; sj["bus"] = s.bus; sj["level"] = s.level; ss.push_back(sj); } bj["sends"] = ss;
		bs.push_back(bj);
	}
	j["buses"] = bs;
	json ms = json::array(); for (const Effect& e : master) ms.push_back(EffectToJson(e)); j["master"] = ms;
	j["masterVolume"] = masterVolume;
	j["spatialMode"] = spatialMode;
	return j.dump(1);
}

bool AudioMixer::LoadFromFile(const std::string& fullPath)
{
	boost::filesystem::ifstream f(boost::filesystem::path(fullPath), std::ios::binary);
	if (!f) return false;
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return LoadFromString(text);
}
bool AudioMixer::SaveToFile(const std::string& fullPath) const
{
	boost::system::error_code ec;
	boost::filesystem::create_directories(boost::filesystem::path(fullPath).parent_path(), ec);
	boost::filesystem::ofstream f(boost::filesystem::path(fullPath), std::ios::binary | std::ios::trunc);
	if (!f) return false;
	f << SaveToString();
	return true;
}

int AudioMixer::BusIndex(const std::string& name) const
{
	for (size_t i = 0; i < buses.size(); ++i) if (buses[i].name == name) return (int)i;
	return -1;
}
int AudioMixer::EffectIndex(int bus, const std::string& type) const
{
	const std::vector<Effect>* chain = bus == -1 ? &master : (bus >= 0 && bus < (int)buses.size() ? &buses[bus].effects : nullptr);
	if (!chain) return -1;
	for (size_t i = 0; i < chain->size(); ++i) if ((*chain)[i].type == type) return (int)i;
	return -1;
}
void AudioMixer::ApplySends(iAudio* a, int bus) const
{
	if (!a || bus < 0 || bus >= (int)buses.size()) return;
	std::vector<NukeAudioSend> ss;
	for (const Send& s : buses[bus].sends)
	{
		const int t = BusIndex(s.bus);
		if (t < 0 || t == bus) continue;
		NukeAudioSend d; d.bus = t; d.level = s.level; ss.push_back(d);
	}
	a->setBusSends(bus, ss.data(), (int)ss.size());
}

static NukeAudioEffect ToPod(const AudioMixer::Effect& e)
{
	NukeAudioEffect o;
	std::strncpy(o.type, e.type.c_str(), sizeof(o.type) - 1); o.type[sizeof(o.type) - 1] = 0;
	o.enabled = e.enabled ? 1 : 0;
	std::memcpy(o.p, e.p, sizeof(o.p));
	return o;
}

static unsigned gApplyCount = 0;
unsigned AudioMixer::ApplyCount() { return gApplyCount; }

void AudioMixer::Apply(iAudio* a) const
{
	if (!a) return;
	++gApplyCount;
	std::vector<NukeAudioBusDesc> bd;
	for (const Bus& b : buses)
	{
		NukeAudioBusDesc d;
		std::strncpy(d.name, b.name.c_str(), sizeof(d.name) - 1); d.name[sizeof(d.name) - 1] = 0;
		d.parent = b.parent; d.volume = b.volume;
		bd.push_back(d);
	}
	a->setMixer(bd.data(), (int)bd.size());
	for (size_t i = 0; i < buses.size(); ++i)
	{
		std::vector<NukeAudioEffect> fx; for (const Effect& e : buses[i].effects) fx.push_back(ToPod(e));
		a->setBusEffects((int)i, fx.data(), (int)fx.size());
		ApplySends(a, (int)i);
	}
	std::vector<NukeAudioEffect> mfx; for (const Effect& e : master) mfx.push_back(ToPod(e));
	a->setBusEffects(-1, mfx.data(), (int)mfx.size());
	a->setMasterVolume(masterVolume);
	a->setSpatialMode(spatialMode);
}

AudioMixer& AudioMixer::Current()
{
	if (!gLoaded)
	{
		gLoaded = true;
		gCurrent = Default();
		gCurrentPath.clear();
		AppInstance* app = AppInstance::GetSingleton();
		const std::string rel = app ? app->audioMixerPath : std::string();
		if (!rel.empty())
		{
			std::string text;
			const std::string full = app->ResolveContent(rel);
			boost::system::error_code ec;
			bool ok = false;
			if (!full.empty() && boost::filesystem::exists(boost::filesystem::path(full), ec)) ok = gCurrent.LoadFromFile(full);
			else if (app->ReadContent(rel, text) && !text.empty()) ok = gCurrent.LoadFromString(text);   // packed
			if (ok) { gCurrentPath = rel; std::cout << "[Audio]\t\tmixer " << rel << " (" << gCurrent.buses.size() << " buses)" << std::endl; }
			else gCurrent = Default();
		}
	}
	return gCurrent;
}
const std::string& AudioMixer::CurrentPath() { Current(); return gCurrentPath; }

bool AudioMixer::Load(const std::string& contentRel)
{
	AppInstance* app = AppInstance::GetSingleton();
	AudioMixer m;
	std::string text;
	const std::string full = app ? app->ResolveContent(contentRel) : contentRel;
	boost::system::error_code ec;
	bool ok = false;
	if (!full.empty() && boost::filesystem::exists(boost::filesystem::path(full), ec)) ok = m.LoadFromFile(full);
	else if (app && app->ReadContent(contentRel, text) && !text.empty()) ok = m.LoadFromString(text);
	if (!ok) return false;
	gLoaded = true; gCurrent = m; gCurrentPath = contentRel; gApplied = false;
	if (iAudio* a = GetService<iAudio>()) { if (a->init()) { m.Apply(a); gApplied = true; gAppliedTo = a; } }
	return true;
}
void AudioMixer::Set(const AudioMixer& m, bool apply)
{
	gLoaded = true; gCurrent = m;
	if (!apply) { gApplied = false; return; }
	if (iAudio* a = GetService<iAudio>()) { if (a->init()) { gCurrent.Apply(a); gApplied = true; gAppliedTo = a; } }
}
void AudioMixer::EnsureApplied(iAudio* a)
{
	if (!a) return;
	AudioMixer& m = Current();
	if (gApplied && gAppliedTo == a) return;
	m.Apply(a); gApplied = true; gAppliedTo = a;
}
void AudioMixer::Invalidate() { gLoaded = false; gApplied = false; }

}  // namespace nuke
