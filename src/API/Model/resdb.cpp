#include "API/Model/resdb.h"
#include "API/Model/Prefab.h"   // PrefabGuid (register prefab guid<->path)
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
			s->rendererHandle = s->isPost ? r->createPostPipeline(s->name.c_str(), s->psSource.c_str())
			                              : r->createShaderPipeline(s->vsSource.c_str(), s->psSource.c_str());
			std::cout << "[ResDB]\t" << (s->isPost ? "post" : "shader") << " pipeline '" << s->name
			          << "' -> handle " << s->rendererHandle << std::endl;
		}
}

void ResDB::HotReloadShaders(iRender* r)
{
	if (!r) return;
	boost::system::error_code ec;
	for (Shader* s : shaders)
	{
		if (!s) continue;
		if (s->isPost)   // post-process effect shader (single ".post.hlsl"; no VS pair)
		{
			if (s->psPath.empty()) continue;
			std::time_t pt = bfs::last_write_time(bfs::path(s->psPath), ec); if (ec) { ec.clear(); continue; }
			if (pt == s->psTime) continue;   // unchanged
			Shader* fresh = Shader::LoadPostShader(s->name, s->psPath);
			if (!fresh) continue;
			s->psSource = fresh->psSource; s->psTime = fresh->psTime;
			s->props    = fresh->props;    // re-parsed PostParams (a param may have been added/removed)
			delete fresh;
			uint64_t h = r->createPostPipeline(s->name.c_str(), s->psSource.c_str());
			if (h) { s->rendererHandle = h; std::cout << "[ResDB]\thot-reloaded post shader '" << s->name << "' -> handle " << h << std::endl; }
			continue;
		}
		if (s->vsPath.empty()) continue;
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
		// Post-process effect shader: a single "<name>.post.hlsl" (fullscreen PS; paired with the built-in
		// post.vs by the renderer). Registered as a Shader asset with isPost = true.
		const std::string posuf = ".post.hlsl";
		if (fn.size() > posuf.size() && fn.compare(fn.size() - posuf.size(), posuf.size(), posuf) == 0)
		{
			std::string pname = fn.substr(0, fn.size() - posuf.size());
			if (shaderByGuid.count(pname)) continue;
			Shader* ps = Shader::LoadPostShader(pname, it->path().string());
			if (ps) { RegisterShader(ps); SetAssetPath(pname, it->path().string());
				std::cout << "[ResDB]\tloaded post shader '" << pname << "' (" << ps->props.size() << " params)" << std::endl; }
			continue;
		}
		if (fn.size() <= vsuf.size() || fn.compare(fn.size() - vsuf.size(), vsuf.size(), vsuf) != 0)
			continue;                                   // not a "*.vs.hlsl"
		std::string name = fn.substr(0, fn.size() - vsuf.size());
		if (name == "ui" || name == "shadow" || name == "sky" || name == "post" || name.rfind("outline", 0) == 0) continue;  // renderer-internal passes, not material shaders
		bfs::path psPath = it->path().parent_path() / (name + ".ps.hlsl");
		if (!bfs::exists(psPath, ec)) continue;         // no matching pixel shader
		if (shaderByGuid.count(name)) continue;         // first one wins (engine before project)
		Shader* s = Shader::LoadPair(name, it->path().string(), psPath.string());
		if (!s) { std::cout << "[ResDB]\tfailed to load shader '" << name << "'" << std::endl; continue; }
		RegisterShader(s);
		SetAssetPath(name, it->path().string());   // .vs.hlsl path (for locate/DnD)
		std::cout << "[ResDB]\tloaded shader '" << name << "' (vs " << s->vsSource.size()
		          << " / ps " << s->psSource.size() << " bytes, " << s->props.size() << " props)" << std::endl;
	}
}

void ResDB::SetAssetPath(const std::string& guid, const std::string& path)
{
	if (guid.empty() || path.empty()) return;
	pathByGuid[guid] = path;
	guidByPath[path] = guid;
}
void ResDB::MoveAssetPath(const std::string& oldPath, const std::string& newPath)
{
	auto it = guidByPath.find(oldPath);
	if (it == guidByPath.end()) return;          // not a tracked asset file
	std::string g = it->second;
	guidByPath.erase(it);
	pathByGuid[g]      = newPath;
	guidByPath[newPath] = g;
}
std::string ResDB::PathForGuid(const std::string& guid) const
{
	auto it = pathByGuid.find(guid);
	return it != pathByGuid.end() ? it->second : std::string();
}
std::string ResDB::GuidForPath(const std::string& path) const
{
	auto it = guidByPath.find(path);
	return it != guidByPath.end() ? it->second : std::string();
}

void ResDB::HotReloadAssets(iRender* r)
{
	boost::system::error_code ec;
	// Textures: same Texture* is shared by every material that resolves it, so reloading pixels +
	// dropping the GPU cache refreshes the image EVERYWHERE (templates + live instances). Skip RTs.
	for (auto& kv : texByGuid)
	{
		Texture* t = kv.second; if (!t || t->renderTexture) continue;
		std::string p = PathForGuid(kv.first); if (p.empty() || !bfs::exists(p, ec)) continue;
		long long mt = (long long)bfs::last_write_time(p, ec); if (ec) continue;
		auto mit = assetMtime.find(p);
		if (mit == assetMtime.end()) { assetMtime[p] = mt; continue; }   // first sight: record only
		if (mit->second == mt) continue;
		assetMtime[p] = mt;
		if (Texture* fresh = Texture::LoadFromFile(p))
		{
			t->width = fresh->width; t->height = fresh->height; t->format = fresh->format;
			t->mipCount = fresh->mipCount; t->pixels = std::move(fresh->pixels);
			delete fresh;
			if (r) r->invalidateTexture(t);
			std::cout << "[ResDB]\thot-reloaded texture " << bfs::path(p).filename().string() << std::endl;
		}
	}
	// Materials: reload the ResDB template + re-Resolve (binds shader/textures). Instances are clones.
	for (auto& kv : matByGuid)
	{
		Material* m = kv.second; if (!m) continue;
		std::string p = PathForGuid(kv.first); if (p.empty() || !bfs::exists(p, ec)) continue;
		long long mt = (long long)bfs::last_write_time(p, ec); if (ec) continue;
		auto mit = assetMtime.find(p);
		if (mit == assetMtime.end()) { assetMtime[p] = mt; continue; }
		if (mit->second == mt) continue;
		assetMtime[p] = mt;
		if (Material* fresh = Material::LoadFromFile(p))
		{
			m->color = fresh->color; m->emissive = fresh->emissive;
			m->metallic = fresh->metallic; m->roughness = fresh->roughness; m->emissiveIntensity = fresh->emissiveIntensity;
			m->shaderGuid   = fresh->shaderGuid;
			m->diffuseGuid  = fresh->diffuseGuid; m->normalGuid = fresh->normalGuid; m->specularGuid = fresh->specularGuid;
			m->metalRoughGuid = fresh->metalRoughGuid; m->occlusionGuid = fresh->occlusionGuid; m->emissiveGuid = fresh->emissiveGuid;
			delete fresh;
			m->Resolve();
			std::cout << "[ResDB]\thot-reloaded material " << bfs::path(p).filename().string() << std::endl;
		}
	}
}

void ResDB::CreateRenderTextures(iRender* r)
{
	if (!r) return;
	for (Texture* t : textures)
		if (t && t->renderTexture && t->rtId == 0 && t->width > 0 && t->height > 0)
			t->rtId = r->createRenderTarget(t->width, t->height);
}

void ResDB::RemoveByGuid(const std::string& guid)
{
	if (guid.empty()) return;
	if (auto it = meshByGuid.find(guid);   it != meshByGuid.end())   { meshes.remove(it->second);    meshByGuid.erase(it); }
	if (auto it = matByGuid.find(guid);    it != matByGuid.end())    { materials.remove(it->second);  matByGuid.erase(it); }
	if (auto it = texByGuid.find(guid);    it != texByGuid.end())    { textures.remove(it->second);   texByGuid.erase(it); }
	if (auto it = shaderByGuid.find(guid); it != shaderByGuid.end()) { shaders.remove(it->second);    shaderByGuid.erase(it); }
	if (auto it = pathByGuid.find(guid);   it != pathByGuid.end())   { guidByPath.erase(it->second);  pathByGuid.erase(it); }
}

void ResDB::UnlinkGuid(const std::string& guid)
{
	if (guid.empty()) return;
	for (Material* m : materials)   // only materials hold guid refs (shader + textures)
	{
		if (!m) continue;
		bool ch = false;
		if (m->shaderGuid   == guid) { m->shaderGuid = "world"; ch = true; }
		if (m->diffuseGuid  == guid) { m->diffuseGuid.clear();  ch = true; }
		if (m->normalGuid   == guid) { m->normalGuid.clear();   ch = true; }
		if (m->specularGuid == guid) { m->specularGuid.clear(); ch = true; }
		if (ch) m->Resolve();
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
			SetAssetPath(m->guid, it->path().string());
			std::cout << "[ResDB]\tloaded mesh '" << m->name << "' (" << m->guid << ")" << std::endl;
		}
		else if (ext == ".numat")
		{
			Material* mt = Material::LoadFromFile(it->path().string());
			if (!mt) { std::cout << "[ResDB]\tfailed to load " << it->path().filename().string() << std::endl; continue; }
			if (mt->guid.empty() || matByGuid.count(mt->guid)) { delete mt; continue; }
			RegisterMaterial(mt);
			SetAssetPath(mt->guid, it->path().string());
			std::cout << "[ResDB]\tloaded material '" << mt->matName << "' (" << mt->guid << ")" << std::endl;
		}
		else if (ext == ".nutex")
		{
			Texture* tx = Texture::LoadFromFile(it->path().string());
			if (!tx) { std::cout << "[ResDB]\tfailed to load " << it->path().filename().string() << std::endl; continue; }
			if (tx->guid.empty() || texByGuid.count(tx->guid)) { delete tx; continue; }
			RegisterTexture(tx);
			SetAssetPath(tx->guid, it->path().string());
			std::cout << "[ResDB]\tloaded texture '" << tx->guid << "' (" << tx->width << "x" << tx->height << ")" << std::endl;
		}
		else if (ext == ".nuprefab")
		{
			std::string g = PrefabGuid(it->path().string());   // guid<->path so instances can resolve their prefab
			if (!g.empty()) SetAssetPath(g, it->path().string());
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