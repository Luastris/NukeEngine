#include "API/Model/resdb.h"
#include <cstdlib>   // getenv (NUKE_UPGRADE_ASSETS)
#include <cstring>
#include <functional>
#include "API/Model/Atom.h"          // live-world clone refresh on material hot reload
#include "API/Model/World.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Package.h"   // packed-content scan (3.2)
#include "API/Model/Storage.h"   // the scan's pak reads ride the IO provider (Fast loading 4)
#include "API/Model/Texture.h"
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include "API/Model/Jobs.h"      // Stopping(): background scans bail on shutdown
#include "API/Model/Prefab.h"   // PrefabGuid (register prefab guid<->path)
#include "render/irender.h"
#include "interface/AppInstance.h"   // GuidForContentPath: content root + ResolveContent
#include "input/Input.h"         // .nuinput content -> gameplay input system
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
	Mesh* cyl   = Mesh::CreateCylinder(); cyl->guid  = "builtin:cylinder"; RegisterMesh(cyl);
	Mesh* caps  = Mesh::CreateCapsule();  caps->guid = "builtin:capsule";  RegisterMesh(caps);
	// Built-in foliage placeholders (7.4): the scatter works with zero imported assets.
	Mesh* grass = Mesh::CreateGrassClump(); grass->guid = "builtin:grassclump"; RegisterMesh(grass);
	Mesh* bush  = Mesh::CreateBush();       bush->guid  = "builtin:bush";       RegisterMesh(bush);
	Mesh* tree  = Mesh::CreateTree();       tree->guid  = "builtin:tree";       RegisterMesh(tree);

	// Default material (white) so a MeshRenderer always has something to point at.
	Material* def = new Material();
	def->guid = "builtin:default";
	def->matName = "Default";
	RegisterMaterial(def);
	// Green foliage default: the builtin grass/bush/tree meshes carry no texture, so the
	// material color is the whole look.
	Material* grn = new Material();
	grn->guid = "builtin:grass";
	grn->matName = "Grass";
	grn->color = Color(0.24, 0.52, 0.16, 1.0);
	RegisterMaterial(grn);
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

void ResDB::UnregisterTexture(Texture* t)
{
	if (!t) return;
	textures.erase(std::remove(textures.begin(), textures.end(), t), textures.end());
	if (!t->guid.empty())
	{
		auto it = texByGuid.find(t->guid);
		if (it != texByGuid.end() && it->second == t) texByGuid.erase(it);
	}
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

void ResDB::RegisterClip(AnimClip* c)
{
	if (!c) return;
	clips.push_back(c);
	if (!c->guid.empty()) clipByGuid[c->guid] = c;
}

AnimClip* ResDB::GetClip(const std::string& guid)
{
	auto it = clipByGuid.find(guid);
	return (it != clipByGuid.end()) ? it->second : nullptr;
}

void ResDB::RegisterSkeleton(Skeleton* sk)
{
	if (!sk) return;
	skeletons.push_back(sk);
	if (!sk->guid.empty()) skelByGuid[sk->guid] = sk;
}

Skeleton* ResDB::GetSkeleton(const std::string& guid)
{
	auto it = skelByGuid.find(guid);
	return (it != skelByGuid.end()) ? it->second : nullptr;
}

AnimClip* ResDB::GetClipByName(const std::string& name)
{
	for (AnimClip* c : clips)
		if (c && c->name == name) return c;
	return nullptr;
}

void ResDB::RegisterBoneMap(BoneMap* b)
{
	if (!b) return;
	boneMaps.push_back(b);
	if (!b->guid.empty()) boneMapByGuid[b->guid] = b;
}

void ResDB::RegisterAnimSM(AnimSM* m)
{
	if (!m) return;
	animSMs.push_back(m);
	if (!m->guid.empty()) smByGuid[m->guid] = m;
}

AnimSM* ResDB::GetAnimSM(const std::string& guid)
{
	auto it = smByGuid.find(guid);
	return (it != smByGuid.end()) ? it->second : nullptr;
}

void ResDB::RegisterBlendSpace(BlendSpace* b)
{
	if (!b) return;
	blendSpaces.push_back(b);
	if (!b->guid.empty()) blendByGuid[b->guid] = b;
}

BlendSpace* ResDB::GetBlendSpace(const std::string& guid)
{
	auto it = blendByGuid.find(guid);
	return (it != blendByGuid.end()) ? it->second : nullptr;
}

void ResDB::RegisterSequence(Sequence* s)
{
	if (!s) return;
	sequences.push_back(s);
	if (!s->guid.empty()) seqByGuid[s->guid] = s;
}

Sequence* ResDB::GetSequence(const std::string& guid)
{
	auto it = seqByGuid.find(guid);
	return (it != seqByGuid.end()) ? it->second : nullptr;
}

void ResDB::RegisterRagdoll(RagdollDef* r)
{
	if (!r) return;
	ragdolls.push_back(r);
	if (!r->guid.empty()) ragByGuid[r->guid] = r;
}

RagdollDef* ResDB::GetRagdoll(const std::string& guid)
{
	auto it = ragByGuid.find(guid);
	return (it != ragByGuid.end()) ? it->second : nullptr;
}

BoneMap* ResDB::GetBoneMap(const std::string& guid)
{
	auto it = boneMapByGuid.find(guid);
	return (it != boneMapByGuid.end()) ? it->second : nullptr;
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
			                              : (!s->hsSource.empty() && !s->dsSource.empty()
			                                 ? r->createShaderPipelineTess(s->name.c_str(), s->vsSource.c_str(), s->psSource.c_str(), s->hsSource.c_str(), s->dsSource.c_str())
			                                 : r->createShaderPipeline(s->name.c_str(), s->vsSource.c_str(), s->psSource.c_str()));
			std::cout << "[ResDB]\t" << (s->isPost ? "post" : "shader") << " pipeline '" << s->name
			          << "' -> handle " << s->rendererHandle << std::endl;
		}
}

int ResDB::BuildShaderPipelinesStep(iRender* r, int maxCount)
{
	if (!r) return 0;
	int built = 0, left = 0;
	for (Shader* s : shaders)
	{
		if (!s || s->rendererHandle != 0) continue;
		if (built >= maxCount) { ++left; continue; }
		s->rendererHandle = s->isPost ? r->createPostPipeline(s->name.c_str(), s->psSource.c_str())
		                              : (!s->hsSource.empty() && !s->dsSource.empty()
			                                 ? r->createShaderPipelineTess(s->name.c_str(), s->vsSource.c_str(), s->psSource.c_str(), s->hsSource.c_str(), s->dsSource.c_str())
			                                 : r->createShaderPipeline(s->name.c_str(), s->vsSource.c_str(), s->psSource.c_str()));
		std::cout << "[ResDB]\t" << (s->isPost ? "post" : "shader") << " pipeline '" << s->name
		          << "' -> handle " << s->rendererHandle << std::endl;
		++built;   // count failures too, so a broken shader can't spin the caller forever
	}
	return left;
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
			s->includeProps = fresh->includeProps;
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
		s->includeProps = fresh->includeProps;
		delete fresh;
		uint64_t h = (!s->hsSource.empty() && !s->dsSource.empty()
			                                 ? r->createShaderPipelineTess(s->name.c_str(), s->vsSource.c_str(), s->psSource.c_str(), s->hsSource.c_str(), s->dsSource.c_str())
			                                 : r->createShaderPipeline(s->name.c_str(), s->vsSource.c_str(), s->psSource.c_str()));
		if (h) { s->rendererHandle = h; std::cout << "[ResDB]\thot-reloaded shader '" << s->name << "' -> handle " << h << std::endl; }
	}
}

// Renderer-internal pass pairs, never material shaders. ONE list for the disk scan and the
// packed scan: extend HERE for new passes.
static bool RendererInternalShader(const std::string& name)
{
	return name == "ui" || name == "shadow" || name == "sky" || name == "post" || name == "debug"
	    || name == "sprite" || name == "sprite_lit" || name.rfind("decal", 0) == 0
	    || name.rfind("outline", 0) == 0 || name.rfind("water", 0) == 0    // water* = 7.5 surface/sim/FFT passes
	    || name == "skin"                                                  // GPU skinning compute (stage 3)
	    || name == "grid"                                                  // analytic editor grid pass
	    || name == "cursor"                                                // software cursor pass
	    || name == "gbuffer"                                               // SSR/TAA prepass
	    || name.rfind("hiz", 0) == 0 || name == "occl"                     // Hi-Z occlusion
	    || name == "meshcost"                                              // mesh-cost debug view
	    || name == "boot";                                                 // startup stand-in world shading
}

void ResDB::LoadShadersDir(const std::string& dir)
{
	boost::system::error_code ec;
	if (!bfs::exists(dir, ec)) return;
	const std::string vsuf = ".vs.hlsl";
	for (bfs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (Jobs::Stopping()) return;   // let Shutdown's join return
		if (bfs::is_directory(it->path())) continue;
		std::string fn = it->path().filename().string();
		// Post-process effect shader: a single "<name>.post.hlsl" (the renderer pairs it with post.vs).
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
		if (RendererInternalShader(name)) continue;     // renderer-internal passes, not material shaders
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

// Normalize a path for form-insensitive comparison: generic slashes + case-folded, and cut
// down to the content-relative tail when the path lives under the content root.
static std::string NormForContent(const std::string& p, const std::string& contentRoot)
{
	std::string g = bfs::path(p).generic_string();
	if (!contentRoot.empty())
	{
		std::string cr = bfs::path(contentRoot).generic_string();
		if (!cr.empty() && cr.back() != '/') cr += '/';
#ifdef _WIN32
		if (g.size() > cr.size() && _strnicmp(g.c_str(), cr.c_str(), cr.size()) == 0)
#else
		if (g.size() > cr.size() && strncasecmp(g.c_str(), cr.c_str(), cr.size()) == 0)
#endif
			g = g.substr(cr.size());
	}
	for (char& c : g) c = (char)std::tolower((unsigned char)c);
	return g;
}

std::string ResDB::GuidForContentPath(const std::string& contentRel) const
{
	if (contentRel.empty()) return std::string();
	// Fast paths first: the exact reference, then the resolved absolute form.
	std::string g = GuidForPath(contentRel);
	if (!g.empty()) return g;
	AppInstance* app = AppInstance::GetSingleton();
	const std::string abs = app->ResolveContent(contentRel);
	if (!abs.empty())
	{
		g = GuidForPath(abs);
		if (!g.empty()) return g;
	}
	// Form-insensitive fallback: compare content-relative normalized tails.
	const std::string want = NormForContent(contentRel, std::string());
	for (const auto& kv : guidByPath)
		if (NormForContent(kv.first, app->contentRoot) == want)
			return kv.second;
	return std::string();
}

// Every serialized .numat field, template -> destination IN PLACE (renderer/surface driver
// hold Material pointers across frames — never delete/re-clone). Used for both the ResDB
// template and the live-world MeshRenderer clones so saved edits reach standing surfaces.
static void ApplyMatTemplate(Material* m, const Material* fresh)
{
	m->matName = fresh->matName;
	m->color = fresh->color; m->emissive = fresh->emissive;
	m->metallic = fresh->metallic; m->roughness = fresh->roughness; m->specular = fresh->specular;
	m->emissiveIntensity = fresh->emissiveIntensity;
	m->shaderGuid   = fresh->shaderGuid;
	m->diffuseGuid  = fresh->diffuseGuid; m->normalGuid = fresh->normalGuid; m->specularGuid = fresh->specularGuid;
	m->metalRoughGuid = fresh->metalRoughGuid; m->occlusionGuid = fresh->occlusionGuid; m->emissiveGuid = fresh->emissiveGuid;
	m->metallicGuid = fresh->metallicGuid; m->roughnessGuid = fresh->roughnessGuid; m->opacityGuid = fresh->opacityGuid;
	m->wipeGuid = fresh->wipeGuid; m->wipeThreshold = fresh->wipeThreshold; m->wipeFeather = fresh->wipeFeather;
	m->castShadows = fresh->castShadows; m->receiveShadows = fresh->receiveShadows;
	m->blendMode = fresh->blendMode; m->alphaCutoff = fresh->alphaCutoff;
	m->uvTiling = fresh->uvTiling; m->uvOffset = fresh->uvOffset; m->uvRotation = fresh->uvRotation;
	m->detailGuid = fresh->detailGuid; m->detailNormalGuid = fresh->detailNormalGuid;
	m->detailTiling = fresh->detailTiling; m->detailStrength = fresh->detailStrength;
	m->triplanar = fresh->triplanar; m->vcolorMode = fresh->vcolorMode;
	m->clearCoat = fresh->clearCoat; m->clearCoatRoughness = fresh->clearCoatRoughness;
	m->anisotropy = fresh->anisotropy; m->flowGuid = fresh->flowGuid;
	m->sheen = fresh->sheen; m->sheenTint = fresh->sheenTint;
	m->translucency = fresh->translucency; m->translucencyTint = fresh->translucencyTint;
	m->ior = fresh->ior; m->refractive = fresh->refractive;
	m->iridescence = fresh->iridescence; m->iridescenceThickness = fresh->iridescenceThickness;
	m->liveStates = fresh->liveStates; m->liveLayers = fresh->liveLayers; m->liveHits = fresh->liveHits;
	m->liveFoliage = fresh->liveFoliage; m->liveTweens = fresh->liveTweens;
	m->liveMasks = fresh->liveMasks;   m->liveEvents = fresh->liveEvents;
	m->liveSound = fresh->liveSound;   m->liveSurface = fresh->liveSurface;
	m->physTag = fresh->physTag; m->liveFriction = fresh->liveFriction; m->liveBounce = fresh->liveBounce;
	m->Resolve();
	m->PushRenderProps();
}

void ResDB::HotReloadAssets(iRender* r)
{
	boost::system::error_code ec;
	// Textures: the same Texture* is shared by every material that resolves it, so reloading
	// pixels + dropping the GPU cache refreshes the image everywhere. Skip render targets.
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
	// Materials: reload the ResDB template, then push the SAME copy into every live-world
	// clone of it (MeshRenderer instances) — a saved .numat must reach surfaces already
	// standing in the world, not only future clones.
	std::vector<Material*> changed;
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
			ApplyMatTemplate(m, fresh);
			delete fresh;
			changed.push_back(m);
			std::cout << "[ResDB]\thot-reloaded material " << bfs::path(p).filename().string() << std::endl;
		}
	}
	AppInstance* app = AppInstance::GetSingleton();
	if (!changed.empty() && app && app->currentWorld)
	{
		// In-place field copy (never delete/re-clone): renderer + surface driver hold these
		// Material pointers across frames.
		std::function<void(bc::list<Atom*>&)> walk = [&](bc::list<Atom*>& atoms)
		{
			for (Atom* a : atoms)
			{
				if (!a) continue;
				for (Component* c : a->components)
				{
					if (!c) continue;
					if (std::strcmp(c->name, "MeshRenderer") != 0 && std::strcmp(c->name, "SkinnedMeshRenderer") != 0) continue;
					MeshRenderer* mr = (MeshRenderer*)c;
					for (Material* t : changed)
					{
						if (mr->mat && mr->mat->guid == t->guid) ApplyMatTemplate(mr->mat, t);
						for (Material* sm : mr->mats)
							if (sm && sm->guid == t->guid) ApplyMatTemplate(sm, t);
					}
				}
				walk(a->children);
			}
		};
		walk(app->currentWorld->GetHierarchy());
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
	if (auto it = clipByGuid.find(guid);   it != clipByGuid.end())   { clips.remove(it->second);      clipByGuid.erase(it); }
	if (auto it = skelByGuid.find(guid);   it != skelByGuid.end())   { skeletons.remove(it->second);  skelByGuid.erase(it); }
	if (auto it = boneMapByGuid.find(guid); it != boneMapByGuid.end()) { boneMaps.remove(it->second); boneMapByGuid.erase(it); }
	if (auto it = smByGuid.find(guid);     it != smByGuid.end())     { animSMs.remove(it->second);    smByGuid.erase(it); }
	if (auto it = blendByGuid.find(guid);  it != blendByGuid.end())  { blendSpaces.remove(it->second); blendByGuid.erase(it); }
	if (auto it = seqByGuid.find(guid);    it != seqByGuid.end())    { sequences.remove(it->second);  seqByGuid.erase(it); }
	if (auto it = ragByGuid.find(guid);    it != ragByGuid.end())    { ragdolls.remove(it->second);   ragByGuid.erase(it); }
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

// ---- parallel content scan --------------------------------------------------------------------
// The heavy binary assets (textures, meshes, clips, skeletons) decode on the Jobs pool — file
// read / pak inflate / BC heal / float blobs all scale per core — and only the REGISTRATION
// (the maps, the logs) runs on the scanning thread, in scan order. Everything else keeps the
// serial per-type ladders below.
namespace {
struct ScanItem { std::string rel, disk; };   // disk set = a file on disk; else a pak entry read by rel
struct ScanDecoded { Mesh* mesh = nullptr; Texture* tex = nullptr; AnimClip* clip = nullptr; Skeleton* skel = nullptr; bool heavy = false; };
bool HeavyExt(const std::string& ext) { return ext == ".nutex" || ext == ".numesh" || ext == ".nuanim" || ext == ".nuskel"; }
// Everything LoadContentEntry registers. Other pak entries (terrain bakes, audio, scripts,
// worlds) are read by their own systems on demand — inflating them here would only cost boot time.
bool EntryExt(const std::string& ext)
{
	static const char* kExts[] = { ".numesh", ".numat", ".nutex", ".nuinput", ".nuanim", ".nuskel",
	                               ".nubonemap", ".nusm", ".nublend", ".nuseq", ".nurag", ".nuprefab" };
	for (const char* e : kExts) if (ext == e) return true;
	return false;
}
}

void ResDB::LoadContentItems(const std::vector<std::pair<std::string, std::string>>& items)
{
	const int n = (int)items.size();
	std::vector<ScanDecoded> dec(n);

	// Phase 1 — bytes of the packed heavy entries through the Storage provider (DirectStorage:
	// the whole scan becomes one NVMe request queue with GPU inflate). Cooked textures skip it:
	// with a GPU-texture provider they load header-only and their mips stream into VRAM later.
	std::vector<std::string> bytes(n);
	std::vector<char> served(n, 0), okRead(n, 0), headerOnly(n, 0);
	std::vector<Package::Location> locs(n);
	const bool gpuTex = Storage::GpuTextures();
	boost::mutex waitLock; boost::condition_variable waitCv; int pending = 0;
	for (int i = 0; i < n; ++i)
	{
		const std::string& rel = items[i].first; const std::string& disk = items[i].second;
		if (!disk.empty()) continue;
		const std::string ext = bfs::path(rel).extension().string();
		if (!HeavyExt(ext)) continue;
		if (ext == ".nutex" && gpuTex && Package::Locate(rel, locs[i]) && locs[i].entry.layout == Texture::kPakLayout)
		{ headerOnly[i] = 1; continue; }
		{ boost::mutex::scoped_lock l(waitLock); ++pending; }
		if (!Storage::TryProvider(rel, Storage::Normal, [&, i](bool ok, std::string& b)
			{
				bytes[i].swap(b); okRead[i] = ok ? 1 : 0;
				boost::mutex::scoped_lock l(waitLock); --pending; waitCv.notify_all();
			}))
		{ boost::mutex::scoped_lock l(waitLock); --pending; continue; }
		served[i] = 1;
	}
	Storage::Flush();
	{
		boost::mutex::scoped_lock l(waitLock);
		while (pending > 0) waitCv.wait(l);   // the provider completes on its own thread
	}

	// Phase 2 — decode on the pool (disk files and unserved pak entries read right here).
	Jobs::ParallelFor(0, n, 1, [&](int i)
	{
		if (Jobs::Stopping()) return;
		const std::string& rel = items[i].first; const std::string& disk = items[i].second;
		const std::string ext = bfs::path(disk.empty() ? rel : disk).extension().string();
		if (!HeavyExt(ext)) return;
		ScanDecoded& d = dec[i]; d.heavy = true;
		if (headerOnly[i]) { d.tex = Texture::LoadFromPak(locs[i]); return; }
		if (disk.empty())
		{
			if (served[i] ? !okRead[i] : !Package::Read(rel, bytes[i])) return;
		}
		if (ext == ".nutex")       d.tex  = disk.empty() ? Texture::LoadFromMemory(bytes[i])  : Texture::LoadFromFile(disk);
		else if (ext == ".numesh") d.mesh = disk.empty() ? Mesh::LoadFromMemory(bytes[i])     : Mesh::LoadFromFile(disk);
		else if (ext == ".nuanim") d.clip = disk.empty() ? AnimClip::LoadFromMemory(bytes[i]) : AnimClip::LoadFromFile(disk);
		else if (ext == ".nuskel") d.skel = disk.empty() ? Skeleton::LoadFromMemory(bytes[i]) : Skeleton::LoadFromFile(disk);
		std::string().swap(bytes[i]);
	});
	for (int i = 0; i < n; ++i)
	{
		if (Jobs::Stopping()) return;   // let Shutdown's join return
		const std::string& rel = items[i].first; const std::string& disk = items[i].second;
		ScanDecoded& d = dec[i];
		if (!d.heavy)
		{
			if (!disk.empty()) { LoadContentFile(disk); continue; }
			if (!EntryExt(bfs::path(rel).extension().string())) continue;
			std::string bytes;
			if (Package::Read(rel, bytes)) LoadContentEntry(rel, bytes);
			continue;
		}
		const std::string shown = disk.empty() ? rel : bfs::path(disk).filename().string();
		if (d.mesh)
		{
			if (d.mesh->guid.empty() || meshByGuid.count(d.mesh->guid)) { delete d.mesh; continue; }
			RegisterMesh(d.mesh);
			if (!disk.empty()) SetAssetPath(d.mesh->guid, disk);
			std::cout << "[ResDB]	loaded mesh '" << d.mesh->name << "' (" << (disk.empty() ? "pak" : d.mesh->guid) << ")" << std::endl;
		}
		else if (d.tex)
		{
			if (d.tex->guid.empty()) { delete d.tex; continue; }
			if (texByGuid.count(d.tex->guid))
			{
				// A silently skipped duplicate never registers, so anything referencing it by path
				// stays invisible with no error: log loudly.
				std::cout << "[ResDB]	DUPLICATE GUID '" << d.tex->guid << "': '" << shown
				          << "' collides with '" << PathForGuid(d.tex->guid) << "' - file SKIPPED (re-import or re-save one of them)" << std::endl;
				delete d.tex; continue;
			}
			RegisterTexture(d.tex);
			if (!disk.empty()) SetAssetPath(d.tex->guid, disk);
			std::cout << "[ResDB]	loaded texture '" << d.tex->guid << "' (" << d.tex->width << "x" << d.tex->height
			          << (disk.empty() ? (d.tex->pakSource ? ", pak, VRAM-direct" : ", pak") : "") << ")" << std::endl;
			if (d.tex->pakSource)
				if (Storage::Provider* p = Storage::GetProvider()) p->PrefetchTexture(d.tex);
			// A pre-v11 file was HEALED in memory (a BC re-encode, the slowest thing in the scan):
			// write the healed v11 back so the next boot reads it as is. Editor, or a dev player
			// run with NUKE_UPGRADE_ASSETS=1 — a shipped game never writes into its content.
			if (d.tex->healedOnLoad && !disk.empty()
			    && (AppInstance::GetSingleton()->isEditor() || std::getenv("NUKE_UPGRADE_ASSETS")))
			{
				if (d.tex->SaveToFile(disk)) std::cout << "[ResDB]	upgraded '" << shown << "' to texture v11 (healed mips saved)" << std::endl;
			}
		}
		else if (d.clip)
		{
			if (d.clip->guid.empty() || clipByGuid.count(d.clip->guid)) { delete d.clip; continue; }
			RegisterClip(d.clip);
			if (!disk.empty()) SetAssetPath(d.clip->guid, disk);
			std::cout << "[ResDB]	loaded clip '" << d.clip->name << "' (" << d.clip->duration << " s)" << std::endl;
		}
		else if (d.skel)
		{
			if (d.skel->guid.empty() || skelByGuid.count(d.skel->guid)) { delete d.skel; continue; }
			RegisterSkeleton(d.skel);
			if (!disk.empty()) SetAssetPath(d.skel->guid, disk);
			std::cout << "[ResDB]	loaded skeleton '" << d.skel->name << "' (" << d.skel->bones.size() << " bones)" << std::endl;
		}
		else
			std::cout << "[ResDB]	failed to load " << shown << std::endl;
	}
}

void ResDB::LoadContentDir(const std::string& dir)
{
	boost::system::error_code ec;
	if (!bfs::exists(dir, ec)) return;
	std::vector<std::pair<std::string, std::string>> items;
	for (bfs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (Jobs::Stopping()) return;   // let Shutdown's join return
		if (bfs::is_directory(it->path())) continue;
		items.push_back({ std::string(), it->path().string() });
	}
	LoadContentItems(items);
}

// Packed runtime: the same registration pass over the Package layer stack. Raw overlay files
// load from disk; pak entries load from MEMORY (no SetAssetPath: there is no file to locate).
void ResDB::LoadContentPackaged()
{
	std::vector<std::pair<std::string, std::string>> items;
	for (const std::string& rel : Package::List("content/"))
	{
		if (Jobs::Stopping()) return;   // let Shutdown's join return
		items.push_back({ rel, Package::ResolveRead(rel) });   // raw overlay wins, else the pak entry
	}
	LoadContentItems(items);
}

// ONE packed entry (project-relative path + raw bytes) -> the DB, from memory.
void ResDB::LoadContentEntry(const std::string& rel, const std::string& bytes)
{
	bfs::path p(rel);
	auto ext = p.extension();
	const std::string stem = p.stem().string();
	if (ext == ".numesh")
	{
		Mesh* m = Mesh::LoadFromMemory(bytes);
		if (!m) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (m->guid.empty() || meshByGuid.count(m->guid)) { delete m; return; }
		RegisterMesh(m);
		std::cout << "[ResDB]	loaded mesh '" << m->name << "' (pak)" << std::endl;
	}
	else if (ext == ".numat")
	{
		Material* mt = Material::LoadFromString(bytes);
		if (!mt) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (mt->guid.empty() || matByGuid.count(mt->guid)) { delete mt; return; }
		RegisterMaterial(mt);
		std::cout << "[ResDB]	loaded material '" << mt->matName << "' (pak)" << std::endl;
	}
	else if (ext == ".nutex")
	{
		Texture* tx = Texture::LoadFromMemory(bytes);
		if (!tx) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (tx->guid.empty() || texByGuid.count(tx->guid)) { delete tx; return; }
		RegisterTexture(tx);
		std::cout << "[ResDB]	loaded texture '" << tx->guid << "' (pak)" << std::endl;
	}
	else if (ext == ".nuinput")
	{
		std::string mrel = rel;
		if (mrel.rfind("content/", 0) == 0) mrel = mrel.substr(8);
		if (!Input::MapEnabled(mrel))
			std::cout << "[ResDB]	input map (pak) '" << rel << "' skipped (not in the project's Input Maps)" << std::endl;
		else if (Input::LoadAssetFromString(bytes))
			std::cout << "[ResDB]	loaded input map (pak) '" << rel << "'" << std::endl;
	}
	else if (ext == ".nuanim")
	{
		AnimClip* c = AnimClip::LoadFromMemory(bytes);
		if (!c) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (c->guid.empty() || clipByGuid.count(c->guid)) { delete c; return; }
		RegisterClip(c);
		std::cout << "[ResDB]	loaded clip '" << c->name << "' (pak)" << std::endl;
	}
	else if (ext == ".nuskel")
	{
		Skeleton* sk = Skeleton::LoadFromMemory(bytes);
		if (!sk) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (sk->guid.empty() || skelByGuid.count(sk->guid)) { delete sk; return; }
		RegisterSkeleton(sk);
		std::cout << "[ResDB]	loaded skeleton '" << sk->name << "' (pak)" << std::endl;
	}
	else if (ext == ".nubonemap")
	{
		BoneMap* b = BoneMap::LoadFromString(bytes, stem);
		if (!b) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (b->guid.empty() || boneMapByGuid.count(b->guid)) { delete b; return; }
		RegisterBoneMap(b);
	}
	else if (ext == ".nusm")
	{
		AnimSM* m = AnimSM::LoadFromMemory(bytes);
		if (!m) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (m->guid.empty() || smByGuid.count(m->guid)) { delete m; return; }
		RegisterAnimSM(m);
		std::cout << "[ResDB]	loaded anim SM '" << m->name << "' (pak)" << std::endl;
	}
	else if (ext == ".nublend")
	{
		BlendSpace* b = BlendSpace::LoadFromMemory(bytes);
		if (!b) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (b->guid.empty() || blendByGuid.count(b->guid)) { delete b; return; }
		RegisterBlendSpace(b);
		std::cout << "[ResDB]	loaded blend space '" << b->name << "' (pak)" << std::endl;
	}
	else if (ext == ".nuseq")
	{
		Sequence* q = Sequence::LoadFromMemory(bytes);
		if (!q) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (q->guid.empty() || seqByGuid.count(q->guid)) { delete q; return; }
		RegisterSequence(q);
		std::cout << "[ResDB]	loaded sequence '" << q->name << "' (pak)" << std::endl;
	}
	else if (ext == ".nurag")
	{
		RagdollDef* r = RagdollDef::LoadFromMemory(bytes);
		if (!r) { std::cout << "[ResDB]	failed to load (pak) " << rel << std::endl; return; }
		if (r->guid.empty() || ragByGuid.count(r->guid)) { delete r; return; }
		RegisterRagdoll(r);
		std::cout << "[ResDB]	loaded ragdoll '" << r->name << "' (pak)" << std::endl;
	}
	else if (ext == ".nuprefab")
	{
		// guid<->PROJECT-RELATIVE key so instances resolve their prefab through the pak.
		std::string g = PrefabGuidFromString(bytes);
		if (!g.empty() && !pathByGuid.count(g)) SetAssetPath(g, rel);
	}
}

// Shaders from the Package layers: .vs/.ps pairs matched by base name + single .post.hlsl files.
// "content/" = project shaders (all register), "shaders/" = engine built-ins (internal passes skipped).
static void ScanPakShaders(ResDB* db, const std::string& prefix, bool builtins)
{
	std::map<std::string, std::pair<std::string, std::string>> pairs;   // base -> (vsRel, psRel)
	for (const std::string& rel : Package::List(prefix))
	{
		std::string low = rel;
		for (char& c : low) c = (char)tolower((unsigned char)c);
		auto ends = [&](const char* suf) { size_t n = strlen(suf); return low.size() > n && low.compare(low.size() - n, n, suf) == 0; };
		if (ends(".post.hlsl"))
		{
			std::string name = bfs::path(rel).filename().string();
			name = name.substr(0, name.size() - strlen(".post.hlsl"));
			if (db->shaderByGuid.count(name)) continue;
			std::string src;
			if (Package::Read(rel, src))
				if (Shader* sh = Shader::PostFromSource(name, src))
				{ db->RegisterShader(sh); std::cout << "[ResDB]	loaded post shader '" << name << "' (pak)" << std::endl; }
		}
		else if (ends(".vs.hlsl") || ends(".ps.hlsl"))
		{
			std::string fn = bfs::path(rel).filename().string();
			std::string base = fn.substr(0, fn.size() - strlen(".vs.hlsl"));
			if (ends(".vs.hlsl")) pairs[base].first = rel; else pairs[base].second = rel;
		}
	}
	for (auto& kv : pairs)
	{
		if (kv.second.first.empty() || kv.second.second.empty() || db->shaderByGuid.count(kv.first)) continue;
		if (builtins && RendererInternalShader(kv.first)) continue;
		std::string vs, ps;
		if (Package::Read(kv.second.first, vs) && Package::Read(kv.second.second, ps))
			if (Shader* sh = Shader::FromSources(kv.first, vs, ps))
			{ db->RegisterShader(sh); std::cout << "[ResDB]	loaded shader '" << kv.first << "' (pak)" << std::endl; }
	}
}

void ResDB::LoadShadersPackaged()
{
	ScanPakShaders(this, "content/", false);
	ScanPakShaders(this, "shaders/", true);   // engine built-ins ride in the pak too (3.2)
}

// ONE content file -> the DB, dispatched by extension (shared by both scans above).
void ResDB::LoadContentFile(const std::string& path)
{
	bfs::path p(path);
	auto ext = p.extension();
	if (ext == ".numesh")
	{
		Mesh* m = Mesh::LoadFromFile(path);
		if (!m) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (m->guid.empty() || meshByGuid.count(m->guid)) { delete m; return; }   // skip dups
		RegisterMesh(m);
		SetAssetPath(m->guid, path);
		std::cout << "[ResDB]	loaded mesh '" << m->name << "' (" << m->guid << ")" << std::endl;
	}
	else if (ext == ".numat")
	{
		Material* mt = Material::LoadFromFile(path);
		if (!mt) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (mt->guid.empty() || matByGuid.count(mt->guid)) { delete mt; return; }
		RegisterMaterial(mt);
		SetAssetPath(mt->guid, path);
		std::cout << "[ResDB]	loaded material '" << mt->matName << "' (" << mt->guid << ")" << std::endl;
	}
	else if (ext == ".nutex")
	{
		Texture* tx = Texture::LoadFromFile(path);
		if (!tx) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (tx->guid.empty()) { delete tx; return; }
		if (texByGuid.count(tx->guid))
		{
			// A silently skipped duplicate never registers, so anything referencing it by path
			// stays invisible with no error: log loudly.
			std::cout << "[ResDB]	DUPLICATE GUID '" << tx->guid << "': '" << path
			          << "' collides with '" << PathForGuid(tx->guid)
			          << "' - file SKIPPED (re-import or re-save one of them)" << std::endl;
			delete tx; return;
		}
		RegisterTexture(tx);
		SetAssetPath(tx->guid, path);
		std::cout << "[ResDB]	loaded texture '" << tx->guid << "' (" << tx->width << "x" << tx->height << ")" << std::endl;
	}
	else if (ext == ".nuinput")   // input map -> Input system, not a GUID'd asset
	{
		// the project may pin an EXPLICIT map list (.nuproj "inputMaps"); default = all.
		std::string rel = p.generic_string();
		const std::string& root = AppInstance::GetSingleton()->contentRoot;
		if (!root.empty() && rel.rfind(boost::filesystem::path(root).generic_string(), 0) == 0)
		{
			rel = rel.substr(boost::filesystem::path(root).generic_string().size());
			while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(rel.begin());
		}
		if (!Input::MapEnabled(rel))
			std::cout << "[ResDB]	input map '" << p.filename().string() << "' skipped (not in the project's Input Maps)" << std::endl;
		else if (Input::LoadAsset(path))
			std::cout << "[ResDB]	loaded input map '" << p.filename().string() << "'" << std::endl;
	}
	else if (ext == ".nuanim")
	{
		AnimClip* c = AnimClip::LoadFromFile(path);
		if (!c) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (c->guid.empty() || clipByGuid.count(c->guid)) { delete c; return; }
		RegisterClip(c);
		SetAssetPath(c->guid, path);
		std::cout << "[ResDB]	loaded clip '" << c->name << "' (" << c->duration << " s)" << std::endl;
	}
	else if (ext == ".nuskel")
	{
		Skeleton* sk = Skeleton::LoadFromFile(path);
		if (!sk) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (sk->guid.empty() || skelByGuid.count(sk->guid)) { delete sk; return; }
		RegisterSkeleton(sk);
		SetAssetPath(sk->guid, path);
		std::cout << "[ResDB]	loaded skeleton '" << sk->name << "' (" << sk->bones.size() << " bones)" << std::endl;
	}
	else if (ext == ".nubonemap")
	{
		BoneMap* b = BoneMap::LoadFromFile(path);
		if (!b) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (b->guid.empty() || boneMapByGuid.count(b->guid)) { delete b; return; }
		RegisterBoneMap(b);
		SetAssetPath(b->guid, path);
		std::cout << "[ResDB]	loaded bone map '" << b->name << "' (" << b->map.size() << " entries)" << std::endl;
	}
	else if (ext == ".nusm")
	{
		AnimSM* m = AnimSM::LoadFromFile(path);
		if (!m) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (m->guid.empty() || smByGuid.count(m->guid)) { delete m; return; }
		RegisterAnimSM(m);
		SetAssetPath(m->guid, path);
		std::cout << "[ResDB]	loaded anim SM '" << m->name << "' (" << m->layers.size() << " layers)" << std::endl;
	}
	else if (ext == ".nublend")
	{
		BlendSpace* b = BlendSpace::LoadFromFile(path);
		if (!b) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (b->guid.empty() || blendByGuid.count(b->guid)) { delete b; return; }
		RegisterBlendSpace(b);
		SetAssetPath(b->guid, path);
		std::cout << "[ResDB]	loaded blend space '" << b->name << "' (" << b->points.size() << " points)" << std::endl;
	}
	else if (ext == ".nuseq")
	{
		Sequence* q = Sequence::LoadFromFile(path);
		if (!q) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (q->guid.empty() || seqByGuid.count(q->guid)) { delete q; return; }
		RegisterSequence(q);
		SetAssetPath(q->guid, path);
		std::cout << "[ResDB]	loaded sequence '" << q->name << "' (" << q->duration << " s)" << std::endl;
	}
	else if (ext == ".nurag")
	{
		RagdollDef* r = RagdollDef::LoadFromFile(path);
		if (!r) { std::cout << "[ResDB]	failed to load " << p.filename().string() << std::endl; return; }
		if (r->guid.empty() || ragByGuid.count(r->guid)) { delete r; return; }
		RegisterRagdoll(r);
		SetAssetPath(r->guid, path);
		std::cout << "[ResDB]	loaded ragdoll '" << r->name << "' (" << r->bodies.size() << " bodies)" << std::endl;
	}
	else if (ext == ".nuprefab")
	{
		std::string g = PrefabGuid(path);   // guid<->path so instances can resolve their prefab
		if (!g.empty()) SetAssetPath(g, path);
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