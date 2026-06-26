#include "API/Model/resdb.h"
#include <boost/filesystem.hpp>
#include <iostream>

namespace nuke {

namespace bfs = boost::filesystem;

ResDB::ResDB()
{
	// Built-in primitive meshes (procedural geometry, fixed GUIDs).
	Mesh* cube  = Mesh::CreateCube();   cube->guid  = "builtin:cube";   RegisterMesh(cube);
	Mesh* plane = Mesh::CreatePlane();  plane->guid = "builtin:plane";  RegisterMesh(plane);
	Mesh* sphere= Mesh::CreateSphere(); sphere->guid= "builtin:sphere"; RegisterMesh(sphere);

	// Default material (white) so a MeshRenderer always has something to point at.
	Material* def = new Material();
	def->guid = "builtin:default";
	def->matName = "Default";
	RegisterMaterial(def);
}

ResDB* ResDB::getSingleton()
{
	static ResDB instance;
	return &instance;
}

void ResDB::RegisterMesh(Mesh* m)
{
	if (!m) return;
	meshes.push_back(m);
	if (!m->guid.empty()) meshByGuid[m->guid] = m;
}

Mesh* ResDB::GetMesh(const std::string& guid)
{
	auto it = meshByGuid.find(guid);
	return (it != meshByGuid.end()) ? it->second : nullptr;
}

void ResDB::RegisterMaterial(Material* m)
{
	if (!m) return;
	materials.push_back(m);
	if (!m->guid.empty()) matByGuid[m->guid] = m;
}

Material* ResDB::GetMaterial(const std::string& guid)
{
	auto it = matByGuid.find(guid);
	return (it != matByGuid.end()) ? it->second : nullptr;
}

void ResDB::RegisterTexture(Texture* t)
{
	if (!t) return;
	textures.push_back(t);
	if (!t->guid.empty()) texByGuid[t->guid] = t;
}

Texture* ResDB::GetTexture(const std::string& guid)
{
	auto it = texByGuid.find(guid);
	return (it != texByGuid.end()) ? it->second : nullptr;
}

std::string ResDB::NewGuid()
{
	// Random uuid-like id via boost::filesystem (boost-uuid isn't in the vcpkg set).
	return bfs::unique_path("%%%%%%%%-%%%%-%%%%-%%%%-%%%%%%%%%%%%").string();
}

void ResDB::LoadContentDir(const std::string& dir)
{
	boost::system::error_code ec;
	if (!bfs::exists(dir, ec)) return;
	for (bfs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (bfs::is_directory(it->path())) continue;
		auto ext = it->path().extension();
		if (ext == ".numesh")
		{
			Mesh* m = Mesh::LoadFromFile(it->path().string());
			if (!m) { std::cout << "[ResDB]\tfailed to load " << it->path().filename().string() << std::endl; continue; }
			if (m->guid.empty() || meshByGuid.count(m->guid)) { delete m; continue; }   // skip dups
			RegisterMesh(m);
			std::cout << "[ResDB]\tloaded mesh '" << m->name << "' (" << m->guid << ")" << std::endl;
		}
		else if (ext == ".numat")
		{
			Material* mt = Material::LoadFromFile(it->path().string());
			if (!mt) { std::cout << "[ResDB]\tfailed to load " << it->path().filename().string() << std::endl; continue; }
			if (mt->guid.empty() || matByGuid.count(mt->guid)) { delete mt; continue; }
			RegisterMaterial(mt);
			std::cout << "[ResDB]\tloaded material '" << mt->matName << "' (" << mt->guid << ")" << std::endl;
		}
		else if (ext == ".nutex")
		{
			Texture* tx = Texture::LoadFromFile(it->path().string());
			if (!tx) { std::cout << "[ResDB]\tfailed to load " << it->path().filename().string() << std::endl; continue; }
			if (tx->guid.empty() || texByGuid.count(tx->guid)) { delete tx; continue; }
			RegisterTexture(tx);
			std::cout << "[ResDB]\tloaded texture '" << tx->guid << "' (" << tx->width << "x" << tx->height << ")" << std::endl;
		}
	}
}

std::shared_ptr<uint> ResDB::loadTexture(const std::string& name)
{
	Texture* i = nullptr;
	for (auto t : textures) {
		if (strcmp(t->path, name.c_str()) == 0)
			i = t;
	}
	if (i == nullptr) {
		i = new Texture();
	}

	// TODO: return not empty
	return std::shared_ptr<uint>();
}
}  // namespace nuke