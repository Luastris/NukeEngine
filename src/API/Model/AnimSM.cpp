#include "API/Model/AnimSM.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <sstream>

namespace nuke {

namespace bfs = boost::filesystem;

namespace {

nlohmann::json TransToJson(const AnimSM::Transition& t)
{
	nlohmann::json j;
	j["from"] = t.from;
	j["to"]   = t.to;
	if (!t.conds.empty())
	{
		nlohmann::json cs = nlohmann::json::array();
		for (const AnimSM::Cond& c : t.conds)
			cs.push_back({ { "param", c.param }, { "op", c.op }, { "value", c.value } });
		j["conds"] = cs;
	}
	if (t.hasExit)          { j["hasExit"] = true; j["exitTime"] = t.exitTime; }
	j["duration"] = t.duration;
	if (t.mode)             j["mode"] = t.mode;
	if (t.interrupt)        j["interrupt"] = t.interrupt;
	return j;
}

AnimSM::Transition TransFromJson(const nlohmann::json& j)
{
	AnimSM::Transition t;
	t.from     = j.value("from", "");
	t.to       = j.value("to", "");
	if (j.contains("conds"))
		for (const auto& cj : j["conds"])
		{
			AnimSM::Cond c;
			c.param = cj.value("param", "");
			c.op    = cj.value("op", 0);
			c.value = cj.value("value", 0.0f);
			t.conds.push_back(c);
		}
	t.hasExit   = j.value("hasExit", false);
	t.exitTime  = j.value("exitTime", 1.0f);
	t.duration  = j.value("duration", 0.25f);
	t.mode      = j.value("mode", 0);
	t.interrupt = j.value("interrupt", 0);
	return t;
}

nlohmann::json StateToJson(const AnimSM::State& s)
{
	nlohmann::json j;
	j["name"] = s.name;
	if (!s.motion.empty())     j["motion"] = s.motion;
	j["loop"]  = s.loop;
	if (s.speed != 1.0f)       j["speed"] = s.speed;
	if (!s.speedParam.empty()) j["speedParam"] = s.speedParam;
	if (s.mirror)              j["mirror"] = true;
	if (!s.sync.empty())       j["sync"] = s.sync;
	if (s.nx != 0.0f || s.ny != 0.0f) { j["nx"] = s.nx; j["ny"] = s.ny; }
	if (!s.states.empty())
	{
		nlohmann::json cs = nlohmann::json::array();
		for (const AnimSM::State& c : s.states) cs.push_back(StateToJson(c));
		j["states"] = cs;
		nlohmann::json ts = nlohmann::json::array();
		for (const AnimSM::Transition& t : s.transitions) ts.push_back(TransToJson(t));
		j["transitions"] = ts;
		j["entry"] = s.entry;
	}
	return j;
}

AnimSM::State StateFromJson(const nlohmann::json& j)
{
	AnimSM::State s;
	s.name       = j.value("name", "");
	s.motion     = j.value("motion", "");
	s.loop       = j.value("loop", true);
	s.speed      = j.value("speed", 1.0f);
	s.speedParam = j.value("speedParam", "");
	s.mirror     = j.value("mirror", false);
	s.sync       = j.value("sync", "");
	s.nx         = j.value("nx", 0.0f);
	s.ny         = j.value("ny", 0.0f);
	if (j.contains("states"))
		for (const auto& cj : j["states"]) s.states.push_back(StateFromJson(cj));
	if (j.contains("transitions"))
		for (const auto& tj : j["transitions"]) s.transitions.push_back(TransFromJson(tj));
	s.entry = j.value("entry", "");
	return s;
}

}  // namespace

std::string AnimSM::ToString() const
{
	nlohmann::json j;
	j["type"] = "AnimSM";
	j["guid"] = guid;
	j["name"] = name;
	nlohmann::json ps = nlohmann::json::array();
	for (const Param& p : params)
		ps.push_back({ { "name", p.name }, { "type", p.type }, { "def", p.def } });
	j["params"] = ps;
	nlohmann::json ls = nlohmann::json::array();
	for (const Layer& l : layers)
	{
		nlohmann::json lj;
		lj["name"] = l.name;
		if (!l.mask.empty()) lj["mask"] = l.mask;
		if (l.additive)      lj["additive"] = true;
		lj["weight"] = l.weight;
		nlohmann::json ss = nlohmann::json::array();
		for (const State& s : l.states) ss.push_back(StateToJson(s));
		lj["states"] = ss;
		nlohmann::json ts = nlohmann::json::array();
		for (const Transition& t : l.transitions) ts.push_back(TransToJson(t));
		lj["transitions"] = ts;
		lj["entry"] = l.entry;
		ls.push_back(lj);
	}
	j["layers"] = ls;
	return j.dump(1);
}

AnimSM* AnimSM::FromString(const std::string& json)
{
	try
	{
		nlohmann::json j = nlohmann::json::parse(json);
		AnimSM* m = new AnimSM();
		m->guid = j.value("guid", "");
		m->name = j.value("name", "");
		if (j.contains("params"))
			for (const auto& pj : j["params"])
			{
				Param p;
				p.name = pj.value("name", "");
				p.type = pj.value("type", 0);
				p.def  = pj.value("def", 0.0f);
				m->params.push_back(p);
			}
		if (j.contains("layers"))
			for (const auto& lj : j["layers"])
			{
				Layer l;
				l.name     = lj.value("name", "");
				l.mask     = lj.value("mask", "");
				l.additive = lj.value("additive", false);
				l.weight   = lj.value("weight", 1.0f);
				if (lj.contains("states"))
					for (const auto& sj : lj["states"]) l.states.push_back(StateFromJson(sj));
				if (lj.contains("transitions"))
					for (const auto& tj : lj["transitions"]) l.transitions.push_back(TransFromJson(tj));
				l.entry = lj.value("entry", "");
				m->layers.push_back(l);
			}
		return m;
	}
	catch (const std::exception&) { return nullptr; }
}

bool AnimSM::SaveToFile(const std::string& path) const
{
	bfs::ofstream o(bfs::path(path), std::ios::binary);
	if (!o) return false;
	const std::string s = ToString();
	o.write(s.data(), (std::streamsize)s.size());
	return (bool)o;
}

AnimSM* AnimSM::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	std::stringstream ss;
	ss << i.rdbuf();
	return FromString(ss.str());
}

AnimSM* AnimSM::LoadFromMemory(const std::string& data) { return FromString(data); }

}  // namespace nuke
