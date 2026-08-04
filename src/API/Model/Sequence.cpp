#include "API/Model/Sequence.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <sstream>

namespace nuke {

namespace bfs = boost::filesystem;

namespace {

nlohmann::json KeysToJson(const std::vector<AnimClip::Key>& keys, int comps)
{
	nlohmann::json a = nlohmann::json::array();
	for (const AnimClip::Key& k : keys)
	{
		nlohmann::json e = nlohmann::json::array();
		e.push_back(k.t);
		for (int i = 0; i < comps; ++i) e.push_back(k.v[i]);
		a.push_back(e);
	}
	return a;
}

std::vector<AnimClip::Key> KeysFromJson(const nlohmann::json& a)
{
	std::vector<AnimClip::Key> keys;
	if (!a.is_array()) return keys;
	for (const auto& e : a)
	{
		if (!e.is_array() || e.empty()) continue;
		AnimClip::Key k;
		k.t = e[0].get<float>();
		k.v[0] = k.v[1] = k.v[2] = k.v[3] = 0;
		for (size_t i = 1; i < e.size() && i <= 4; ++i) k.v[i - 1] = e[i].get<float>();
		keys.push_back(k);
	}
	return keys;
}

}  // namespace

std::string Sequence::ToString() const
{
	nlohmann::json j;
	j["type"] = "Sequence";
	j["guid"] = guid;
	j["name"] = name;
	j["duration"] = duration;
	nlohmann::json tt = nlohmann::json::array();
	for (const TransformTrack& t : transformTracks)
	{
		nlohmann::json e;
		e["path"] = t.path;
		if (!t.pos.empty())   e["pos"] = KeysToJson(t.pos, 3);
		if (!t.rot.empty())   e["rot"] = KeysToJson(t.rot, 4);
		if (!t.scale.empty()) e["scale"] = KeysToJson(t.scale, 3);
		tt.push_back(e);
	}
	j["transformTracks"] = tt;
	nlohmann::json pt = nlohmann::json::array();
	for (const PropTrack& t : propTracks)
		pt.push_back({ { "path", t.path }, { "comp", t.comp }, { "prop", t.prop },
		               { "dim", t.dim }, { "keys", KeysToJson(t.keys, t.dim <= 0 ? 1 : t.dim) } });
	j["propTracks"] = pt;
	nlohmann::json bt = nlohmann::json::array();
	for (const BoneTrack& t : boneTracks)
	{
		nlohmann::json e;
		e["path"] = t.path;
		e["bone"] = t.bone;
		if (!t.pos.empty()) e["pos"] = KeysToJson(t.pos, 3);
		if (!t.rot.empty()) e["rot"] = KeysToJson(t.rot, 4);
		bt.push_back(e);
	}
	j["boneTracks"] = bt;
	nlohmann::json ct = nlohmann::json::array();
	for (const ClipTrack& t : clipTracks)
	{
		nlohmann::json es = nlohmann::json::array();
		for (const ClipEntry& e : t.entries)
			es.push_back({ { "t", e.t }, { "clip", e.clip }, { "speed", e.speed } });
		ct.push_back({ { "path", t.path }, { "entries", es } });
	}
	j["clipTracks"] = ct;
	nlohmann::json ev = nlohmann::json::array();
	for (const Event& e : events)
		ev.push_back({ { "t", e.t }, { "name", e.name }, { "payload", e.payload } });
	j["events"] = ev;
	nlohmann::json cu = nlohmann::json::array();
	for (const Cut& c : cuts)
		cu.push_back({ { "t", c.t }, { "camera", c.camera } });
	j["cuts"] = cu;
	nlohmann::json st = nlohmann::json::array();
	for (const Sting& s : stings)
		st.push_back({ { "t", s.t }, { "clip", s.clip }, { "volume", s.volume }, { "bus", s.bus } });
	j["stings"] = st;
	return j.dump(1);
}

Sequence* Sequence::FromString(const std::string& json)
{
	try
	{
		nlohmann::json j = nlohmann::json::parse(json);
		Sequence* s = new Sequence();
		s->guid = j.value("guid", "");
		s->name = j.value("name", "");
		s->duration = j.value("duration", 5.0);
		if (j.contains("transformTracks"))
			for (const auto& e : j["transformTracks"])
			{
				TransformTrack t;
				t.path = e.value("path", "");
				if (e.contains("pos"))   t.pos = KeysFromJson(e["pos"]);
				if (e.contains("rot"))   t.rot = KeysFromJson(e["rot"]);
				if (e.contains("scale")) t.scale = KeysFromJson(e["scale"]);
				s->transformTracks.push_back(std::move(t));
			}
		if (j.contains("propTracks"))
			for (const auto& e : j["propTracks"])
			{
				PropTrack t;
				t.path = e.value("path", "");
				t.comp = e.value("comp", "");
				t.prop = e.value("prop", "");
				t.dim = e.value("dim", 1);
				if (e.contains("keys")) t.keys = KeysFromJson(e["keys"]);
				s->propTracks.push_back(std::move(t));
			}
		if (j.contains("boneTracks"))
			for (const auto& e : j["boneTracks"])
			{
				BoneTrack t;
				t.path = e.value("path", "");
				t.bone = e.value("bone", "");
				if (e.contains("pos")) t.pos = KeysFromJson(e["pos"]);
				if (e.contains("rot")) t.rot = KeysFromJson(e["rot"]);
				s->boneTracks.push_back(std::move(t));
			}
		if (j.contains("clipTracks"))
			for (const auto& e : j["clipTracks"])
			{
				ClipTrack t;
				t.path = e.value("path", "");
				if (e.contains("entries"))
					for (const auto& en : e["entries"])
					{
						ClipEntry ce;
						ce.t = en.value("t", 0.0f);
						ce.clip = en.value("clip", "");
						ce.speed = en.value("speed", 1.0f);
						t.entries.push_back(ce);
					}
				s->clipTracks.push_back(std::move(t));
			}
		if (j.contains("events"))
			for (const auto& e : j["events"])
				s->events.push_back({ e.value("t", 0.0f), e.value("name", ""), e.value("payload", "") });
		if (j.contains("cuts"))
			for (const auto& e : j["cuts"])
				s->cuts.push_back({ e.value("t", 0.0f), e.value("camera", "") });
		if (j.contains("stings"))
			for (const auto& e : j["stings"])
				s->stings.push_back({ e.value("t", 0.0f), e.value("clip", ""), e.value("volume", 1.0f), e.value("bus", 1) });
		return s;
	}
	catch (const std::exception&) { return nullptr; }
}

bool Sequence::SaveToFile(const std::string& path) const
{
	bfs::ofstream o(bfs::path(path), std::ios::binary);
	if (!o) return false;
	const std::string s = ToString();
	o.write(s.data(), (std::streamsize)s.size());
	return (bool)o;
}

Sequence* Sequence::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	std::stringstream ss;
	ss << i.rdbuf();
	return FromString(ss.str());
}

Sequence* Sequence::LoadFromMemory(const std::string& data) { return FromString(data); }

}  // namespace nuke
