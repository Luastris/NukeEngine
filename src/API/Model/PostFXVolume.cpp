#include "API/Model/PostFXVolume.h"
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "API/Model/Math.h"   // ScaleExtents / ScaleRadius: the volume follows the atom scale
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace nuke {

static std::vector<PostFXVolume*> gVolumes;   // game thread only

PostFXVolume::PostFXVolume() : Component("PostFXVolume") {}

void PostFXVolume::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	if (std::find(gVolumes.begin(), gVolumes.end(), this) == gVolumes.end()) gVolumes.push_back(this);
}

void PostFXVolume::Destroy()     { gVolumes.erase(std::remove(gVolumes.begin(), gVolumes.end(), this), gVolumes.end()); }
void PostFXVolume::Update()      {}
void PostFXVolume::FixedUpdate() {}
void PostFXVolume::Pause()       {}
void PostFXVolume::Reset()       {}

const std::vector<PostFXVolume*>& PostFXVolume::All() { return gVolumes; }

// The same JSON as PostProcess::EnsureParsed/Commit: [{shader, enabled, props{name:[4]}}].
void PostFXVolume::EnsureParsed()
{
	if (parsedFrom == effectsData) return;
	parsedFrom = effectsData;
	effects.clear();
	if (effectsData.empty()) return;
	nlohmann::json j = nlohmann::json::parse(effectsData, nullptr, false);
	if (j.is_discarded() || !j.is_array()) return;
	for (auto& e : j)
	{
		PostEffect pe;
		pe.shaderGuid = e.value("shader", std::string());
		pe.enabled    = e.value("enabled", true);
		if (e.contains("props") && e["props"].is_object())
			for (auto& kv : e["props"].items())
			{
				std::array<float, 4> v{ 0, 0, 0, 0 };
				if (kv.value().is_array())
					for (int i = 0; i < 4 && i < (int)kv.value().size(); ++i) v[i] = kv.value()[i].get<float>();
				pe.props[kv.key()] = v;
			}
		effects.push_back(std::move(pe));
	}
}

void PostFXVolume::Commit()
{
	nlohmann::json arr = nlohmann::json::array();
	for (const PostEffect& pe : effects)
	{
		nlohmann::json e;
		e["shader"]  = pe.shaderGuid;
		e["enabled"] = pe.enabled;
		nlohmann::json props = nlohmann::json::object();
		for (const auto& kv : pe.props) props[kv.first] = { kv.second[0], kv.second[1], kv.second[2], kv.second[3] };
		e["props"] = props;
		arr.push_back(e);
	}
	effectsData = arr.dump();
	parsedFrom  = effectsData;
}

float PostFXVolume::WeightAt(const Vector3& p) const
{
	if (!transform || !enabled) return 0.0f;
	const Vector3 c = transform->globalPosition();
	const Vector3 scl = transform->globalScale();   // the extents are LOCAL: the atom's scale scales the volume
	double outside;   // metres from the shape's surface, 0 inside
	if (shape == 0)
	{
		const double d = std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y) + (p.z - c.z) * (p.z - c.z));
		outside = std::max(d - (double)ScaleRadius(radius, scl), 0.0);
	}
	else
	{
		// Into the box's local frame (its rotation; the scale is folded into the extents).
		const Vector3 he = ScaleExtents(halfExtents, scl);
		const Quaternion q = transform->globalRotation();
		const Quaternion inv(-q.x, -q.y, -q.z, q.w);
		const Vector3 l = inv.Rotate(Vector3(p.x - c.x, p.y - c.y, p.z - c.z));
		const double dx = std::max(std::fabs(l.x) - std::fabs((double)he.x), 0.0);
		const double dy = std::max(std::fabs(l.y) - std::fabs((double)he.y), 0.0);
		const double dz = std::max(std::fabs(l.z) - std::fabs((double)he.z), 0.0);
		outside = std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	float w = 1.0f;
	if (outside > 0.0) w = (blendDistance > 1e-4f) ? (float)std::max(1.0 - outside / blendDistance, 0.0) : 0.0f;
	return w * std::min(std::max(weight, 0.0f), 1.0f);
}

}  // namespace nuke
