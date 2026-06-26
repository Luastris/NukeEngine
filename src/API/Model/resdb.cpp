#include "API/Model/resdb.h"
#include "render/irender.h"
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

void ResDB::RegisterShader(Shader* s)
{
	if (!s) return;
	shaders.push_back(s);
	if (!s->guid.empty()) shaderByGuid[s->guid] = s;
}

Shader* ResDB::GetShader(const std::string& guid)
{
	auto it = shaderByGuid.find(guid);
	return (it != shaderByGuid.end()) ? it->second : nullptr;
}

void ResDB::BuildShaderPipelines(iRender* r)
{
	if (!r) return;
	for (Shader* s : shaders)
		if (s && s->rendererHandle == 0)
		{
			s->rendererHandle = r->createShaderPipeline(s->vsSource.c_str(), s->psSource.c_str());
			std::cout << "[ResDB]\tshader pipeline '" << s->name << "' -> handle " << s->rendererHandle << std::endl;
		}
}

void ResDB::HotReloadShaders(iRender* r)
{
	if (!r) return;
	boost::system::error_code ec;
	for (Shader* s : shaders)
	{
		if (!s || s->vsPath.empty()) continue;
		std::time_t vt = bfs::last_write_time(bfs::path(s->vsPath), ec); if (ec) { ec.clear(); continue; }
		std::time_t pt = bfs::last_write_time(bfs::path(s->psPath), ec); if (ec) { ec.clear(); continue; }
		if (vt == s->vsTime && pt == s->psTime) continue;   // unchanged
		Shader* fresh = Shader::LoadPair(s->name, s->vsPath, s->psPath);
		if (!fresh) continue;
		s->vsSource = fresh->vsSource; s->psSource = fresh->psSource;
		s->vsTime   = fresh->vsTime;   s->psTime   = fresh->psTime;
		s->props    = fresh->props;    // re-parsed MatCB params (a prop may have been added/removed)
		delete fresh;
		uint64_t h = r->createShaderPipeline(s->vsSource.c_str(), s->psSource.c_str());
		if (h) { s->rendererHandle = h; std::cout << "[ResDB]\thot-reloaded shader '" << s->name << "' -> handle " << h << std::endl; }
	}
}

void ResDB::LoadShadersDir(const std::string& dir)
{
	boost::system::error_code ec;
	if (!bfs::exists(dir, ec)) return;
	const std::string vsuf = ".vs.hlsl";
	for (bfs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (bfs::is_directory(it->path())) continue;
		std::string fn = it->path().filename().string();
		if (fn.size() <= vsuf.size() || fn.compare(fn.size() - vsuf.size(), vsuf.size(), vsuf) != 0)
			continue;                                   // not a "*.vs.hlsl"
		std::string name = fn.substr(0, fn.size() - vsuf.size());
		if (name == "ui") continue;                     // renderer-internal UI pass, not a material shader
		bfs::path psPath = it->path().parent_path() / (name + ".ps.hlsl");
		if (!bfs::exists(psPath, ec)) continue;         // no matching pixel shader
		if (shaderByGuid.count(name)) continue;         // first one wins (engine before project)
		Shader* s = Shader::LoadPair(name, it->path().string(), psPath.string());
		if (!s) { std::cout << "[ResDB]\tfailed to load shader '" << name << "'" << std::endl; continue; }
		RegisterShader(s);
		std::cout << "[ResDB]\tloaded shader '" << name << "' (vs " << s->vsSource.size()
		          << " / ps " << s->psSource.size() << " bytes, " << s->props.size() << " props)" << std::endl;
	}
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