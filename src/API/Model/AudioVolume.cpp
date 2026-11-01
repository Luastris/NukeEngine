// AudioVolume - a listener volume that blends a mixer snapshot and / or its own effects into the live mix.
#include "API/Model/AudioVolume.h"
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "interface/AppInstance.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace nuke {

float VolumeInfluence(Transform* transform, int shape, float radius, const Vector3& halfExtents,
                      float blendDistance, float weight, const Vector3& p)
{
	if (!transform) return 0.0f;
	const Vector3 c = transform->globalPosition();
	const Vector3 scl = transform->globalScale();
	double outside;
	if (shape == 0)
	{
		const double s = std::max(std::fabs((double)scl.x), std::max(std::fabs((double)scl.y), std::fabs((double)scl.z)));
		const double d = std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y) + (p.z - c.z) * (p.z - c.z));
		outside = std::max(d - (double)radius * s, 0.0);
	}
	else
	{
		const Quaternion q = transform->globalRotation();
		const Quaternion inv(-q.x, -q.y, -q.z, q.w);
		const Vector3 l = inv.Rotate(Vector3(p.x - c.x, p.y - c.y, p.z - c.z));
		const double hx = std::fabs((double)halfExtents.x * scl.x), hy = std::fabs((double)halfExtents.y * scl.y), hz = std::fabs((double)halfExtents.z * scl.z);
		const double dx = std::max(std::fabs(l.x) - hx, 0.0), dy = std::max(std::fabs(l.y) - hy, 0.0), dz = std::max(std::fabs(l.z) - hz, 0.0);
		outside = std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	float w = 1.0f;
	if (outside > 0.0) w = (blendDistance > 1e-4f) ? (float)std::max(1.0 - outside / blendDistance, 0.0) : 0.0f;
	return w * std::min(std::max(weight, 0.0f), 1.0f);
}

static std::vector<AudioVolume*> gVolumes;
static std::map<std::string, std::unique_ptr<AudioMixer>> gSnapshots;   // asset path -> loaded mixer (null = missing)
const std::vector<AudioVolume*>& AudioVolume::All() { return gVolumes; }
void AudioVolume::InvalidateSnapshots() { gSnapshots.clear(); }

AudioVolume::AudioVolume() : Component("AudioVolume") {}

void AudioVolume::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	if (std::find(gVolumes.begin(), gVolumes.end(), this) == gVolumes.end()) gVolumes.push_back(this);
}

void AudioVolume::Destroy()
{
	gVolumes.erase(std::remove(gVolumes.begin(), gVolumes.end(), this), gVolumes.end());
}

float AudioVolume::WeightAt(const Vector3& p) const
{
	if (!enabled) return 0.0f;
	return VolumeInfluence(transform, shape, radius, halfExtents, blendDistance, weight, p);
}

// effectsData: [{ "type", "enabled", "params": [8] }] - the mixer's effect JSON.
void AudioVolume::EnsureParsed()
{
	if (parsedFrom == effectsData) return;
	parsedFrom = effectsData;
	effects.clear();
	if (effectsData.empty()) return;
	nlohmann::json j = nlohmann::json::parse(effectsData, nullptr, false);
	if (j.is_discarded() || !j.is_array()) return;
	for (const nlohmann::json& ej : j)
	{
		if (!ej.is_object()) continue;
		AudioMixer::Effect e;
		e.type = ej.value("type", std::string());
		e.enabled = ej.value("enabled", true);
		if (ej.contains("params") && ej["params"].is_array())
		{
			int i = 0;
			for (const nlohmann::json& v : ej["params"]) { if (i >= 8) break; e.p[i++] = v.is_number() ? v.get<float>() : 0.0f; }
		}
		if (!e.type.empty()) effects.push_back(e);
	}
}

void AudioVolume::Commit()
{
	nlohmann::json arr = nlohmann::json::array();
	for (const AudioMixer::Effect& e : effects)
	{
		nlohmann::json ej; ej["type"] = e.type; ej["enabled"] = e.enabled;
		nlohmann::json ps = nlohmann::json::array(); for (float v : e.p) ps.push_back(v); ej["params"] = ps;
		arr.push_back(ej);
	}
	effectsData = arr.dump();
	parsedFrom = effectsData;
}

const AudioMixer* AudioVolume::Snapshot()
{
	if (mixer.empty()) return nullptr;
	auto it = gSnapshots.find(mixer);
	if (it != gSnapshots.end()) return it->second.get();
	std::unique_ptr<AudioMixer> m;
	AppInstance* app = AppInstance::GetSingleton();
	if (app)
	{
		std::unique_ptr<AudioMixer> tmp(new AudioMixer());
		std::string text;
		const std::string full = app->ResolveContent(mixer);
		boost::system::error_code ec;
		bool ok = false;
		if (!full.empty() && boost::filesystem::exists(boost::filesystem::path(full), ec)) ok = tmp->LoadFromFile(full);
		else if (app->ReadContent(mixer, text) && !text.empty()) ok = tmp->LoadFromString(text);
		if (ok) m = std::move(tmp);
	}
	const AudioMixer* out = m.get();
	gSnapshots[mixer] = std::move(m);
	return out;
}

}  // namespace nuke
