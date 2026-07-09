#include "API/Model/BoneMap.h"
#include "API/Model/resdb.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iostream>

namespace nuke {

namespace bfs = boost::filesystem;
using nlohmann::json;

// { "type": "BoneMap", "version": 1, "guid": "...", "map": { "from": "to", ... } }
bool BoneMap::SaveToFile(const std::string& path) const
{
	json j;
	j["type"] = "BoneMap";
	j["version"] = 1;
	j["guid"] = guid;
	json m = json::object();
	for (const auto& kv : map) m[kv.first] = kv.second;
	j["map"] = m;
	bfs::ofstream o{ bfs::path(path) };
	if (!o) return false;
	o << j.dump(2);
	return (bool)o;
}

BoneMap* BoneMap::LoadFromFile(const std::string& path)
{
	bfs::ifstream i{ bfs::path(path) };
	if (!i) return nullptr;
	json j;
	try { i >> j; }
	catch (const std::exception& e)
	{
		std::cout << "[BoneMap]\tbad json in " << path << ": " << e.what() << std::endl;
		return nullptr;
	}
	if (j.value("type", "") != "BoneMap") return nullptr;
	BoneMap* b = new BoneMap();
	b->guid = j.value("guid", "");
	b->name = bfs::path(path).stem().string();
	if (j.contains("map") && j["map"].is_object())
		for (auto it = j["map"].begin(); it != j["map"].end(); ++it)
			if (it.value().is_string()) b->map[it.key()] = it.value().get<std::string>();
	return b;
}

std::string BoneMap::Template()
{
	json j;
	j["type"] = "BoneMap";
	j["version"] = 1;
	j["guid"] = ResDB::NewGuid();
	j["map"] = { { "sourceBoneName", "targetBoneName" } };
	return j.dump(2) + "\n";
}

}  // namespace nuke
