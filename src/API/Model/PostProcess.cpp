#include "API/Model/PostProcess.h"
#include "API/Model/Atom.h"
#include <nlohmann/json.hpp>

namespace nuke {

PostProcess::PostProcess() : Component("PostProcess") {}

void PostProcess::Init(Atom* parent)
{
	transform = &parent->GetTransform();   // same pointer as the sibling Camera component on this atom
	atom = parent;
	parent->components.push_back(this);
}

void PostProcess::Destroy()     {}
void PostProcess::Update()       {}
void PostProcess::FixedUpdate()  {}
void PostProcess::Pause()        {}
void PostProcess::Reset()        {}

void PostProcess::EnsureParsed()
{
	if (parsedFrom == effectsData) return;   // up to date
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

void PostProcess::Commit()
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

}  // namespace nuke
