#include "API/Model/Skeleton.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <iostream>

namespace nuke {

namespace bfs = boost::filesystem;
using json = nlohmann::json;

int Skeleton::BoneIndex(const std::string& boneName) const
{
	for (size_t i = 0; i < bones.size(); ++i)
		if (bones[i].name == boneName) return (int)i;
	return -1;
}

const SkeletonSocket* Skeleton::Socket(const std::string& socketName) const
{
	for (const SkeletonSocket& s : sockets)
		if (s.name == socketName) return &s;
	return nullptr;
}

void Skeleton::GroupMask(const std::string& groupName, std::vector<float>& out) const
{
	out.assign(bones.size(), 0.0f);
	for (const SkeletonGroup& g : groups)
		if (g.name == groupName)
		{
			for (const std::string& b : g.bones)
			{
				int i = BoneIndex(b);
				if (i >= 0) out[i] = 1.0f;
			}
			return;
		}
}

// ---- JSON (.nuskel) -------------------------------------------------------------------------

std::string Skeleton::ToString() const
{
	json j;
	j["type"]    = "Skeleton";
	j["version"] = 1;
	j["guid"]    = guid;
	j["name"]    = name;
	json jb = json::array();
	for (const MeshBone& b : bones)
	{
		json e;
		e["name"]   = b.name;
		e["parent"] = b.parent;
		e["pos"]    = { b.localPos[0], b.localPos[1], b.localPos[2] };
		e["rot"]    = { b.localRot[0], b.localRot[1], b.localRot[2], b.localRot[3] };
		e["scale"]  = { b.localScale[0], b.localScale[1], b.localScale[2] };
		json inv = json::array();
		for (int k = 0; k < 16; ++k) inv.push_back(b.invBind[k]);
		e["invBind"] = inv;
		jb.push_back(e);
	}
	j["bones"] = jb;
	json js = json::array();
	for (const SkeletonSocket& s : sockets)
	{
		json e;
		e["name"]  = s.name;
		e["bone"]  = s.bone;
		e["pos"]   = { s.localPos[0], s.localPos[1], s.localPos[2] };
		e["rot"]   = { s.localRot[0], s.localRot[1], s.localRot[2], s.localRot[3] };
		e["scale"] = { s.localScale[0], s.localScale[1], s.localScale[2] };
		js.push_back(e);
	}
	j["sockets"] = js;
	json jg = json::array();
	for (const SkeletonGroup& g : groups) jg.push_back({ { "name", g.name }, { "bones", g.bones } });
	j["groups"] = jg;
	json jc = json::array();
	for (const SkeletonChain& c : chains) jc.push_back({ { "name", c.name }, { "bones", c.bones } });
	j["chains"] = jc;
	return j.dump(1);
}

Skeleton* Skeleton::FromString(const std::string& data)
{
	json j = json::parse(data, nullptr, false);
	if (j.is_discarded() || j.value("type", "") != "Skeleton") return nullptr;
	Skeleton* s = new Skeleton();
	s->guid = j.value("guid", "");
	s->name = j.value("name", "");
	auto vec3 = [](const json& a, float* out, float d0, float d1, float d2)
	{
		out[0] = d0; out[1] = d1; out[2] = d2;
		if (a.is_array() && a.size() >= 3) for (int k = 0; k < 3; ++k) out[k] = a[k].get<float>();
	};
	auto vec4 = [](const json& a, float* out)
	{
		out[0] = out[1] = out[2] = 0; out[3] = 1;
		if (a.is_array() && a.size() >= 4) for (int k = 0; k < 4; ++k) out[k] = a[k].get<float>();
	};
	for (const json& e : j.value("bones", json::array()))
	{
		MeshBone b;
		b.name   = e.value("name", "");
		b.parent = e.value("parent", -1);
		vec3(e.value("pos", json()), b.localPos, 0, 0, 0);
		vec4(e.value("rot", json()), b.localRot);
		vec3(e.value("scale", json()), b.localScale, 1, 1, 1);
		const json& inv = e.value("invBind", json());
		for (int k = 0; k < 16; ++k)
			b.invBind[k] = (inv.is_array() && (int)inv.size() == 16) ? inv[k].get<float>() : (k % 5 == 0 ? 1.0f : 0.0f);
		// Hierarchy-order invariant: a parent must precede its child (forward pass relies on it).
		if (b.parent >= (int)s->bones.size()) b.parent = -1;
		s->bones.push_back(b);
	}
	for (const json& e : j.value("sockets", json::array()))
	{
		SkeletonSocket k;
		k.name = e.value("name", "");
		k.bone = e.value("bone", "");
		vec3(e.value("pos", json()), k.localPos, 0, 0, 0);
		vec4(e.value("rot", json()), k.localRot);
		vec3(e.value("scale", json()), k.localScale, 1, 1, 1);
		s->sockets.push_back(k);
	}
	for (const json& e : j.value("groups", json::array()))
	{
		SkeletonGroup g;
		g.name = e.value("name", "");
		for (const json& b : e.value("bones", json::array())) g.bones.push_back(b.get<std::string>());
		s->groups.push_back(g);
	}
	for (const json& e : j.value("chains", json::array()))
	{
		SkeletonChain c;
		c.name = e.value("name", "");
		for (const json& b : e.value("bones", json::array())) c.bones.push_back(b.get<std::string>());
		s->chains.push_back(c);
	}
	return s;
}

bool Skeleton::SaveToFile(const std::string& path) const
{
	bfs::ofstream o(bfs::path(path), std::ios::binary);
	if (!o) return false;
	const std::string data = ToString();
	o.write(data.data(), (std::streamsize)data.size());
	return (bool)o;
}

Skeleton* Skeleton::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	std::stringstream ss; ss << i.rdbuf();
	return FromString(ss.str());
}

Skeleton* Skeleton::LoadFromMemory(const std::string& data) { return FromString(data); }

}  // namespace nuke
