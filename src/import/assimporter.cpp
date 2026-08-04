#include "import/assimporter.h"
#include "interface/Importers.h"   // plugin importer registry (dispatched from ImportAny)
#include "API/Model/Prefab.h"
#include "API/Model/Transform.h"
#include "API/Model/Texture.h"
#include "API/Model/AnimClip.h"
#include "API/Model/Animator.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Skeleton.h"
#include "API/Model/Ragdoll.h"
#include <boost/filesystem.hpp>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <boost/filesystem/fstream.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include "API/Model/Jobs.h"        // async import (2.4)
#include "API/Model/StatusBar.h"   // live import status
#include <boost/atomic.hpp>
#include <boost/thread/mutex.hpp>

namespace nuke {

// Live import progress (worker -> status bar). Installed only by an ASYNC import;
// a synchronous game-thread import leaves it null.
struct ImportProgress
{
	std::string key;    // StatusBar entry id (unique per queued import)
	std::string name;   // source filename (label prefix)
	int done  = 0;      // completed units
	int total = 0;      // 0 until the scene was counted -> indeterminate bar
};
static thread_local ImportProgress* tlProg = nullptr;

// Report the current unit's label + its inner fraction [0..1].
static void ProgStage(const std::string& label, float sub = 0.0f)
{
	if (!tlProg) return;
	if (tlProg->total <= 0)
	{
		StatusBar::Set(tlProg->key, tlProg->name + " — " + label, StatusBar::kIndeterminate);
		return;
	}
	if (sub < 0.0f) sub = 0.0f;
	if (sub > 1.0f) sub = 1.0f;
	StatusBar::Set(tlProg->key, tlProg->name + " — " + label,
	               ((float)tlProg->done + sub) / (float)tlProg->total);
}
static void ProgUnitDone() { if (tlProg) ++tlProg->done; }

// BC-compress one RGBA level (any size; partial edge blocks clamp) -> appended to `out`. 8=BC1, 16=BC3.
static void BCLevel(std::vector<unsigned char>& out, const unsigned char* rgba, int w, int h, int blockBytes, int alpha)
{
	const int bx = (w + 3) / 4, by = (h + 3) / 4;
	size_t base = out.size();
	out.resize(base + (size_t)bx * by * blockBytes);
	unsigned char* dst = out.data() + base;
	for (int byi = 0; byi < by; ++byi)
		for (int bxi = 0; bxi < bx; ++bxi)
		{
			unsigned char block[64];
			for (int py = 0; py < 4; ++py)
				for (int px = 0; px < 4; ++px)
				{
					int sx = bxi * 4 + px; if (sx >= w) sx = w - 1;
					int sy = byi * 4 + py; if (sy >= h) sy = h - 1;
					const unsigned char* s = &rgba[((size_t)sy * w + sx) * 4];
					unsigned char* d = &block[(py * 4 + px) * 4];
					d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
				}
			stb_compress_dxt_block(dst, block, alpha, STB_DXT_NORMAL);
			dst += blockBytes;
		}
}

// Edge-replicate pad an RGBA image up to a multiple of 4 (so any size can be BC-compressed). Returns the
// padded buffer + the padded dims; if already aligned, returns a copy at the same size.
static std::vector<unsigned char> PadTo4(const std::vector<unsigned char>& rgba0, int w0, int h0, int& wOut, int& hOut)
{
	wOut = (w0 + 3) & ~3; hOut = (h0 + 3) & ~3;
	if (wOut == w0 && hOut == h0) return rgba0;
	std::vector<unsigned char> p((size_t)wOut * hOut * 4);
	for (int y = 0; y < hOut; ++y)
	{
		int sy = y < h0 ? y : h0 - 1;
		for (int x = 0; x < wOut; ++x)
		{
			int sx = x < w0 ? x : w0 - 1;
			const unsigned char* s = &rgba0[((size_t)sy * w0 + sx) * 4];
			unsigned char* d = &p[((size_t)y * wOut + x) * 4];
			d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
		}
	}
	return p;
}

// BC5 (RG) block level for normal maps: a BC4 block of R then one of G (16B per 4x4).
// Blue is dropped; z is reconstructed in the shader as sqrt(1-x^2-y^2).
static void BC5Level(std::vector<unsigned char>& out, const unsigned char* rgba, int w, int h)
{
	const int bx = (w + 3) / 4, by = (h + 3) / 4;
	size_t base = out.size();
	out.resize(base + (size_t)bx * by * 16);
	unsigned char* dst = out.data() + base;
	for (int byi = 0; byi < by; ++byi)
		for (int bxi = 0; bxi < bx; ++bxi)
		{
			unsigned char R[16], G[16];
			for (int py = 0; py < 4; ++py)
				for (int px = 0; px < 4; ++px)
				{
					int sx = bxi * 4 + px; if (sx >= w) sx = w - 1;
					int sy = byi * 4 + py; if (sy >= h) sy = h - 1;
					const unsigned char* s = &rgba[((size_t)sy * w + sx) * 4];
					R[py * 4 + px] = s[0]; G[py * 4 + px] = s[1];
				}
			stb_compress_bc4_block(dst,     R);   // BC4(R) -> 8 bytes
			stb_compress_bc4_block(dst + 8, G);   // BC4(G) -> 8 bytes
			dst += 16;
		}
}

// Compress an RGBA image to BC1/BC3/BC5 + mip chain; non-multiple-of-4 sizes are edge-padded.
// `prog` (optional) receives the compression fraction [0..1].
static void CompressToBC(Texture* tex, const std::vector<unsigned char>& rgba0, int w0, int h0,
                         int usage = Texture::UsageColor,
                         const boost::function<void(float)>& prog = boost::function<void(float)>())
{
	if (w0 <= 0 || h0 <= 0) { tex->format = Texture::FMT_RGBA8; tex->mipCount = 1; tex->width = w0; tex->height = h0; tex->pixels = rgba0; return; }
	const bool bc5 = (usage == Texture::UsageNormal);   // normal maps -> BC5 (RG), z reconstructed in-shader
	bool hasA = false;
	if (!bc5) for (size_t i = 3; i < rgba0.size(); i += 4) if (rgba0[i] < 255) { hasA = true; break; }
	const int blockBytes = bc5 ? 16 : (hasA ? 16 : 8), alpha = hasA ? 1 : 0;
	tex->format = bc5 ? Texture::FMT_BC5 : (hasA ? Texture::FMT_BC3 : Texture::FMT_BC1);
	tex->pixels.clear();

	int w, h;
	std::vector<unsigned char> cur = PadTo4(rgba0, w0, h0, w, h);   // top level padded to a multiple of 4
	tex->width = w; tex->height = h;
	double totalPx = 0.0, donePx = 0.0;
	if (prog)
	{
		for (int tw = w, th = h;;)   // pixel total across the whole mip chain
		{
			totalPx += (double)tw * th;
			if (tw == 1 && th == 1) break;
			tw = tw > 1 ? tw / 2 : 1; th = th > 1 ? th / 2 : 1;
		}
		prog(0.0f);
	}
	int mips = 0;
	while (true)
	{
		if (bc5) BC5Level(tex->pixels, cur.data(), w, h);
		else     BCLevel(tex->pixels, cur.data(), w, h, blockBytes, alpha);
		++mips;
		if (prog) { donePx += (double)w * h; prog((float)(donePx / totalPx)); }
		if (w == 1 && h == 1) break;
		const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
		std::vector<unsigned char> nx((size_t)nw * nh * 4);
		for (int y = 0; y < nh; ++y)
			for (int x = 0; x < nw; ++x)
			{
				int x0 = x * 2, y0 = y * 2, x1 = (x * 2 + 1 < w) ? x * 2 + 1 : x0, y1 = (y * 2 + 1 < h) ? y * 2 + 1 : y0;
				const unsigned char* s0 = &cur[((size_t)y0 * w + x0) * 4]; const unsigned char* s1 = &cur[((size_t)y0 * w + x1) * 4];
				const unsigned char* s2 = &cur[((size_t)y1 * w + x0) * 4]; const unsigned char* s3 = &cur[((size_t)y1 * w + x1) * 4];
				unsigned char* d = &nx[((size_t)y * nw + x) * 4];
				if (alpha)
				{
					// alpha-weighted rgb: plain averaging bleeds transparent texels' black into the mips
					const int aS = s0[3] + s1[3] + s2[3] + s3[3];
					for (int c = 0; c < 3; ++c)
						d[c] = aS ? (unsigned char)((s0[c] * s0[3] + s1[c] * s1[3] + s2[c] * s2[3] + s3[c] * s3[3]) / aS)
						          : (unsigned char)((s0[c] + s1[c] + s2[c] + s3[c]) / 4);
					d[3] = (unsigned char)(aS / 4);
				}
				else
					for (int c = 0; c < 4; ++c) d[c] = (unsigned char)((s0[c] + s1[c] + s2[c] + s3[c]) / 4);
			}
		cur.swap(nx); w = nw; h = nh;
	}
	tex->mipCount = mips;
}

namespace bfs = boost::filesystem;

AssImporter::AssImporter() {}
AssImporter::~AssImporter() {}

Atom* AssImporter::ImportObject(aiNode* node, const aiScene* scene) {
	ResDB* res = ResDB::getSingleton();
	auto atom = new Atom(node->mName.C_Str());
	if (node->mNumMeshes > 0)
	{
		// All of the node's meshes merge into ONE sectioned mesh; material slots dedup per node.
		std::vector<aiMesh*> nodeMeshes;
		bool skinned = false;
		for (unsigned int i = 0; i < node->mNumMeshes; ++i)
		{
			nodeMeshes.push_back(scene->mMeshes[node->mMeshes[i]]);
			skinned = skinned || nodeMeshes.back()->HasBones();
		}
		std::vector<unsigned int> slotMats;
		Mesh* m = Mesh::ImportAIMeshes(nodeMeshes, scene, &slotMats);
		res->meshes.push_back(m);

		// Live path keeps the classic MeshRenderer: its meshes embed their skeleton (no
		// .nuskel asset exists here), which is exactly the Animator's legacy contract.
		(void)skinned;
		MeshRenderer* mr = new MeshRenderer();
		atom->AddComponent(mr);   // Init (ResolveMaterials) runs here — fill the slots AFTER it
		mr->mesh = m;
		for (size_t s = 0; s < slotMats.size(); ++s)
		{
			Material* mat = new Material();
			mat->ImportAiMaterial(scene->mMaterials[slotMats[s]]);
			res->materials.push_back(mat);
			if (s == 0) mr->mat = mat->Clone();   // owned instance; the asset stays the shared template
			mr->mats.push_back(mat->Clone());
		}
	}
	for (int i = 0; i < node->mNumChildren; i++) {
		atom->AddChild(ImportObject(node->mChildren[i], scene));
	}
	return atom;
}

void AssImporter::Import(const char* path) {

	Assimp::Importer importer;
	ResDB* res = ResDB::getSingleton();
	importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);   // metres, see ImportToContent
	const aiScene* sc = importer.ReadFile(path,
	                                      aiProcessPreset_TargetRealtime_MaxQuality | aiProcess_GlobalScale);
	if (!sc)
	{
		cout << importer.GetErrorString() << endl;
		return;
	}

	std::cout << sc->HasAnimations() << " " << sc->mNumAnimations << std::endl;
	std::cout << sc->HasCameras() << " " << sc->mNumCameras << std::endl;
	std::cout << sc->HasLights() << " " << sc->mNumLights << std::endl;
	std::cout << sc->HasMaterials() << " " << sc->mNumMaterials << std::endl;
	std::cout << sc->HasMeshes() << " " << sc->mNumMeshes << std::endl;
	std::cout << sc->HasTextures() << " " << sc->mNumTextures << std::endl;

	if (sc->HasTextures()) {
		cout << "SCENE HAS TExtURES" << endl;
		for (int i = 0; i < sc->mNumTextures; ++i) {
			//cout << "TNAME: " << sc->mTextures[i]->mFilename.C_Str() << endl;
		}
	}
	res->prefabs.push_back(ImportObject(sc->mRootNode, sc));

}

// Sanitize a mesh name into a safe file stem.
static std::string SafeStem(const char* in)
{
	std::string s = (in && in[0]) ? in : "mesh";
	for (char& c : s)
		if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
	return s;
}

// One written .numesh per NODE (its meshes merged into material sections); nodes sharing the
// same mesh set (file-level instancing) share one asset. Key = sorted source-mesh indices.
struct NodeMeshOut
{
	std::string guid;                    // written .numesh asset
	std::vector<unsigned int> slotMats;  // material SLOT -> aiScene material index
	bool skinned = false;
	std::string skelGuid;                // the file's shared .nuskel (skinned nodes)
};

static std::string NodeKey(const aiNode* n)
{
	std::vector<unsigned int> ids(n->mMeshes, n->mMeshes + n->mNumMeshes);
	std::sort(ids.begin(), ids.end());
	std::string k;
	for (unsigned int i : ids) { k += std::to_string(i); k += '_'; }
	return k;
}

static bool SubtreeHasMesh(const aiNode* n)
{
	if (n->mNumMeshes > 0) return true;
	for (unsigned int i = 0; i < n->mNumChildren; ++i)
		if (SubtreeHasMesh(n->mChildren[i])) return true;
	return false;
}

// Rebuild the assimp node tree as an Atom hierarchy: per-node transform, ONE MeshRenderer on
// the node itself (sectioned mesh + per-slot materials by GUID), and an Animator wired to
// `firstClipGuid` on skinned nodes. NO bone atoms: joint-only subtrees live in the .nuskel
// (the pose palette is in the SkinnedMeshRenderer), so a `boneNames` child that carries no
// mesh anywhere below is skipped entirely. Joints with real geometry underneath (weapon
// bones, skinned attachments) are kept so the child's accumulated transform stays honest.
static Atom* BuildPrefabNode(aiNode* node, const aiScene* sc,
                             const std::map<std::string, NodeMeshOut>& nodeMeshes,
                             const std::vector<std::string>& matGuids,
                             const std::set<std::string>& boneNames,
                             const std::string& firstClipGuid = std::string())
{
	Atom* atom = new Atom(node->mName.C_Str());

	aiVector3D pos, scl; aiQuaternion rot;
	node->mTransformation.Decompose(scl, rot, pos);
	Transform& t = atom->GetTransform();
	t.position.x = pos.x; t.position.y = pos.y; t.position.z = pos.z;
	t.rotation.x = rot.x; t.rotation.y = rot.y; t.rotation.z = rot.z; t.rotation.w = rot.w;
	t.scale.x    = scl.x; t.scale.y    = scl.y; t.scale.z    = scl.z;

	if (node->mNumMeshes > 0)
	{
		auto it = nodeMeshes.find(NodeKey(node));
		if (it != nodeMeshes.end())
		{
			MeshRenderer* mr;
			if (it->second.skinned)
			{
				SkinnedMeshRenderer* sm = new SkinnedMeshRenderer();
				sm->skelGuid = it->second.skelGuid;   // the file's shared .nuskel
				mr = sm;
			}
			else mr = new MeshRenderer();
			mr->meshGuid = it->second.guid;
			for (unsigned int mi : it->second.slotMats)
				mr->matGuids.push_back(mi < matGuids.size() ? matGuids[mi] : std::string());
			if (!mr->matGuids.empty()) mr->matGuid = mr->matGuids[0];   // slot 0 doubles as the classic single ref
			atom->AddComponent(mr);
			// NO per-node Animator: ONE Animator on the prefab ROOT drives every skinned
			// mesh of the shared skeleton (added by the caller after the tree is built).
		}
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i)
	{
		aiNode* ch = node->mChildren[i];
		if (boneNames.count(ch->mName.C_Str()) && !SubtreeHasMesh(ch))
			continue;   // joint-only subtree: it lives in the .nuskel, not the prefab
		atom->AddChild(BuildPrefabNode(ch, sc, nodeMeshes, matGuids, boneNames, firstClipGuid));
	}
	return atom;
}

// Decode a texture (external file, or assimp-embedded "*N") to RGBA8 and write a .nutex asset.
// Returns the new texture GUID, or "" if it couldn't be loaded. Dedups within one import.
static std::string ConvertTexture(const aiScene* sc, const std::string& texRef,
                                  const bfs::path& modelDir, const std::string& destDir,
                                  std::map<std::string, std::string>& cache, int usage = Texture::UsageColor)
{
	if (texRef.empty()) return std::string();

	// One progress unit per call on every exit path — the pre-scan counts cache hits too.
	struct UnitGuard { ~UnitGuard() { ProgUnitDone(); } } unitGuard;
	const std::string texLabel = "texture " +
		(texRef[0] == '*' ? std::string("(embedded)") : bfs::path(texRef).filename().string());
	ProgStage(texLabel);

	std::string key = texRef + "|" + std::to_string(usage);   // same image in two roles -> distinct assets/treatment
	auto cit = cache.find(key);
	if (cit != cache.end()) return cit->second;

	int w = 0, h = 0, n = 0;
	std::vector<unsigned char> rgba;

	if (texRef[0] == '*')   // embedded texture
	{
		int idx = atoi(texRef.c_str() + 1);
		if (idx < 0 || (unsigned)idx >= sc->mNumTextures) return std::string();
		const aiTexture* t = sc->mTextures[idx];
		if (t->mHeight == 0)   // compressed (png/jpg bytes)
		{
			unsigned char* px = stbi_load_from_memory((const unsigned char*)t->pcData, (int)t->mWidth, &w, &h, &n, 4);
			if (!px) return std::string();
			rgba.assign(px, px + (size_t)w * h * 4);
			stbi_image_free(px);
		}
		else                   // uncompressed aiTexel (BGRA)
		{
			w = t->mWidth; h = t->mHeight;
			rgba.resize((size_t)w * h * 4);
			for (size_t i = 0; i < (size_t)w * h; ++i)
			{
				rgba[i * 4 + 0] = t->pcData[i].r;
				rgba[i * 4 + 1] = t->pcData[i].g;
				rgba[i * 4 + 2] = t->pcData[i].b;
				rgba[i * 4 + 3] = t->pcData[i].a;
			}
		}
	}
	else                     // external file (resolve relative to the model)
	{
		boost::system::error_code ec;
		bfs::path full = modelDir / texRef;
		if (!bfs::exists(full, ec)) full = bfs::path(texRef);
		unsigned char* px = stbi_load(full.string().c_str(), &w, &h, &n, 4);
		if (!px) { cout << "[Import]\ttexture not found: " << texRef << endl; return std::string(); }
		rgba.assign(px, px + (size_t)w * h * 4);
		stbi_image_free(px);
	}

	Texture* tex = new Texture();
	tex->guid   = ResDB::NewGuid();
	tex->usage  = (Texture::Usage)usage;   // authoritative from the assimp texture type
	CompressToBC(tex, rgba, w, h, usage,
	             [&texLabel](float f) { ProgStage(texLabel, f); });   // BC1/BC3/BC5 + mip chain

	std::string stem = SafeStem(bfs::path(texRef).stem().string().c_str());
	boost::system::error_code ec;
	bfs::path out = bfs::path(destDir) / (stem + ".nutex");
	for (int k = 1; bfs::exists(out, ec); ++k)
		out = bfs::path(destDir) / (stem + "_" + std::to_string(k) + ".nutex");

	if (!tex->SaveToFile(out.string())) { delete tex; return std::string(); }
	AssImporter::Reg([tex] { ResDB::getSingleton()->RegisterTexture(tex); });   // main-thread when async
	cache[key] = tex->guid;
	cout << "[Import]\twrote " << out.filename().string() << " (" << w << "x" << h << ")" << endl;
	return tex->guid;
}

int AssImporter::ImportToContent(const char* srcPath, const char* destDir)
{
	ProgStage("parsing...");   // assimp is one opaque stage -> indeterminate bar
	Assimp::Importer importer;
	// Collapse FBX pivots: otherwise nodes split into $AssimpFbx$_* pseudo-nodes and animation
	// channels bind to pseudo-node names that differ between files.
	importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
	// UNITS. The engine works in metres. An FBX authored in centimetres (Mixamo, Max, Maya's
	// default) carries UnitScaleFactor = 100 in its metadata: imported raw, a character is a
	// 180-METRE giant that fills the screen and drowns the rasterizer in overdraw. GlobalScale
	// applies that metadata once, at import, to meshes, bones, node transforms and animation
	// keys alike; formats that carry no unit metadata (OBJ, glTF) come in unchanged.
	importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
	const aiScene* sc = importer.ReadFile(srcPath,
	                                      aiProcessPreset_TargetRealtime_MaxQuality | aiProcess_GlobalScale);
	if (!sc)
	{
		cout << "[Import]\t" << importer.GetErrorString() << endl;
		return 0;
	}
	{
		// What the file declared, so an odd-looking size is traceable to its units.
		double unit = 1.0;
		if (sc->mMetaData) { double u = 0.0; if (sc->mMetaData->Get("UnitScaleFactor", u) && u > 0.0) unit = u; }
		if (unit != 1.0)
			cout << "[Import]\tsource units: 1 unit = " << (1.0 / unit) << " m — scaled to metres" << endl;
	}

	// The progress total must mirror the conversion loops below.
	if (tlProg)
	{
		int texUnits = 0;
		for (unsigned int i = 0; i < sc->mNumMaterials; ++i)
		{
			aiMaterial* am = sc->mMaterials[i];
			auto has = [am](aiTextureType t) { aiString s; return am->GetTexture(t, 0, &s) == AI_SUCCESS && s.length > 0; };
			if (has(aiTextureType_DIFFUSE))  ++texUnits;
			if (has(aiTextureType_NORMALS))  ++texUnits;
			if (has(aiTextureType_SPECULAR)) ++texUnits;
			if (has(aiTextureType_METALNESS) || has(aiTextureType_DIFFUSE_ROUGHNESS) || has(aiTextureType_UNKNOWN)) ++texUnits;
			if (has(aiTextureType_AMBIENT_OCCLUSION) || has(aiTextureType_LIGHTMAP)) ++texUnits;
			if (has(aiTextureType_EMISSIVE)) ++texUnits;
		}
		// Mesh units = unique NODE mesh-sets (one .numesh per node; shared sets = one asset).
		std::set<std::string> nodeKeys;
		std::function<void(aiNode*)> countNodes = [&](aiNode* n)
		{
			if (n->mNumMeshes > 0) nodeKeys.insert(NodeKey(n));
			for (unsigned int c = 0; c < n->mNumChildren; ++c) countNodes(n->mChildren[c]);
		};
		countNodes(sc->mRootNode);
		tlProg->done  = 0;
		tlProg->total = texUnits + (int)sc->mNumMaterials + (int)nodeKeys.size()
		              + (int)sc->mNumAnimations + 1;
	}

	boost::system::error_code ec;
	bfs::create_directories(destDir, ec);
	ResDB* res = ResDB::getSingleton();

	bfs::path modelDir = bfs::path(srcPath).parent_path();
	std::map<std::string, std::string> texCache;   // source ref -> texture GUID (dedupe)
	std::vector<std::string> matGuids(sc->mNumMaterials);
	for (unsigned int i = 0; i < sc->mNumMaterials; ++i)
	{
		ProgStage("material " + std::to_string(i + 1) + "/" + std::to_string(sc->mNumMaterials));
		aiMaterial* am = sc->mMaterials[i];
		Material* mt = new Material();
		mt->ImportAiMaterial(am);
		mt->guid = ResDB::NewGuid();

		aiString tp;
		if (am->GetTexture(aiTextureType_DIFFUSE, 0, &tp) == AI_SUCCESS)
			mt->diffuseGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache, Texture::UsageColor);
		tp.Clear();
		if (am->GetTexture(aiTextureType_NORMALS, 0, &tp) == AI_SUCCESS)
			mt->normalGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache, Texture::UsageNormal);
		tp.Clear();
		if (am->GetTexture(aiTextureType_SPECULAR, 0, &tp) == AI_SUCCESS)
			mt->specularGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache, Texture::UsageColor);
		tp.Clear();
		if (am->GetTexture(aiTextureType_METALNESS, 0, &tp) == AI_SUCCESS ||
		    am->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &tp) == AI_SUCCESS ||
		    am->GetTexture(aiTextureType_UNKNOWN, 0, &tp) == AI_SUCCESS)   // glTF packs MR under UNKNOWN
			mt->metalRoughGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache, Texture::UsageData);
		tp.Clear();
		if (am->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &tp) == AI_SUCCESS ||
		    am->GetTexture(aiTextureType_LIGHTMAP, 0, &tp) == AI_SUCCESS)
			mt->occlusionGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache, Texture::UsageData);
		tp.Clear();
		if (am->GetTexture(aiTextureType_EMISSIVE, 0, &tp) == AI_SUCCESS)
			mt->emissiveGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache, Texture::UsageEmissive);
		std::string mstem = SafeStem(mt->matName.empty() ? "material" : mt->matName.c_str());
		bfs::path mout = bfs::path(destDir) / (mstem + ".numat");
		for (int n = 1; bfs::exists(mout, ec); ++n)
			mout = bfs::path(destDir) / (mstem + "_" + std::to_string(n) + ".numat");
		if (mt->SaveToFile(mout.string()))
		{
			const std::string mpath = mout.string();
			AssImporter::Reg([mt, mpath]
			{
				ResDB::getSingleton()->RegisterMaterial(mt);
				ResDB::getSingleton()->SetAssetPath(mt->guid, mpath);
			});   // main-thread when async
			matGuids[i] = mt->guid;
			cout << "[Import]\twrote " << mout.filename().string() << " (" << mt->guid << ")" << endl;
		}
		else { cout << "[Import]\tfailed to write " << mout.filename().string() << endl; delete mt; }
		ProgUnitDone();
	}

	// ONE skeleton per FILE (.nuskel): the merged palette every skinned node-mesh indexes
	// into. Modular characters from several files each carry their own skeleton asset.
	std::vector<MeshBone> sceneBones;
	Mesh::ImportAISkeleton(sc, sceneBones);
	std::set<std::string> boneNames;   // prefab builder skips joint-only subtrees by these
	for (const MeshBone& b : sceneBones) boneNames.insert(b.name);
	std::string skelGuid;
	Skeleton* skAsset = nullptr;   // survives for the auto-.nurag below
	if (!sceneBones.empty())
	{
		ProgStage("skeleton");
		Skeleton* sk = new Skeleton();
		sk->guid  = ResDB::NewGuid();
		sk->name  = SafeStem(bfs::path(srcPath).stem().string().c_str());
		sk->bones = sceneBones;
		bfs::path sout = bfs::path(destDir) / (sk->name + ".nuskel");
		for (int n = 1; bfs::exists(sout, ec); ++n)
			sout = bfs::path(destDir) / (sk->name + "_" + std::to_string(n) + ".nuskel");
		if (sk->SaveToFile(sout.string()))
		{
			const std::string spath = sout.string();
			AssImporter::Reg([sk, spath]
			{
				ResDB::getSingleton()->RegisterSkeleton(sk);
				ResDB::getSingleton()->SetAssetPath(sk->guid, spath);
			});
			skelGuid = sk->guid;
			skAsset = sk;
			cout << "[Import]	wrote " << sout.filename().string() << " (" << sk->bones.size()
			     << " bones)" << endl;
		}
		else { cout << "[Import]	failed to write " << sout.filename().string() << endl; delete sk; }
	}

	// One .numesh per NODE: the node's meshes merge into a sectioned, indexed, auto-LOD'd v4
	// mesh; nodes sharing a mesh set share the asset.
	std::map<std::string, NodeMeshOut> nodeMeshes;
	Mesh* ragMesh = nullptr;   // biggest skinned mesh: the auto-.nurag fit source
	int count = 0, meshOrd = 0;
	std::function<void(aiNode*)> writeNodeMeshes = [&](aiNode* node)
	{
		if (node->mNumMeshes > 0)
		{
			const std::string key = NodeKey(node);
			if (!nodeMeshes.count(key))
			{
				ProgStage("mesh " + std::to_string(++meshOrd));
				std::vector<aiMesh*> src;
				NodeMeshOut rec;
				for (unsigned int i = 0; i < node->mNumMeshes; ++i)
				{
					src.push_back(sc->mMeshes[node->mMeshes[i]]);
					rec.skinned = rec.skinned || src.back()->HasBones();
				}
				const bool useShared = rec.skinned && !skelGuid.empty();
				Mesh* m = Mesh::ImportAIMeshes(src, sc, &rec.slotMats, useShared ? &sceneBones : nullptr);
				m->guid = ResDB::NewGuid();
				if (useShared) { m->skelGuid = skelGuid; rec.skelGuid = skelGuid; }
				// the mesh remembers the materials it came with (v7) — every "show this mesh"
				// path (previews, editor rigs, bare drops) then looks right without a prefab
				for (unsigned int mi : rec.slotMats)
					m->defaultMats.push_back(mi < matGuids.size() ? matGuids[mi] : std::string());
				strncpy(m->name, node->mName.C_Str(), sizeof(m->name) - 1);
				m->name[sizeof(m->name) - 1] = 0;

				std::string stem = SafeStem(node->mName.C_Str());
				bfs::path out = bfs::path(destDir) / (stem + ".numesh");
				for (int n = 1; bfs::exists(out, ec); ++n)
					out = bfs::path(destDir) / (stem + "_" + std::to_string(n) + ".numesh");

				if (!m->SaveToFile(out.string()))
				{
					cout << "[Import]\tfailed to write " << out.filename().string() << endl;
					delete m;
				}
				else
				{
					const std::string mpath = out.string();
					AssImporter::Reg([m, mpath]
					{
						ResDB::getSingleton()->RegisterMesh(m);
						ResDB::getSingleton()->SetAssetPath(m->guid, mpath);
					});   // main-thread when async
					rec.guid = m->guid;
					nodeMeshes[key] = rec;
					if (useShared && m->boneWeight && (!ragMesh || m->numVerts > ragMesh->numVerts))
						ragMesh = m;
					int lod0Tris = 0;
					{
						MeshLOD L0 = m->Lod(0);
						for (int s2 = 0; s2 < L0.sectionCount; ++s2)
							lod0Tris += (int)m->Section(L0.firstSection + s2).indexCount / 3;
					}
					cout << "[Import]\twrote " << out.filename().string() << " (" << m->guid
					     << ", " << m->SectionCount() << " section(s), " << m->LodCount() << " LOD(s), "
					     << m->numVerts << " verts / " << lod0Tris << " tris)" << endl;
					++count;
				}
				ProgUnitDone();
			}
		}
		for (unsigned int i = 0; i < node->mNumChildren; ++i) writeNodeMeshes(node->mChildren[i]);
	};
	writeNodeMeshes(sc->mRootNode);

	// The Unity checkbox: auto-fit a .nurag next to the skeleton (hand-tune later if needed).
	if (skAsset && ragMesh)
	{
		ProgStage("ragdoll");
		if (RagdollDef* rd = RagdollDef::Build(skAsset, ragMesh))
		{
			bfs::path rout = bfs::path(destDir) / (skAsset->name + ".nurag");
			for (int n = 1; bfs::exists(rout, ec); ++n)
				rout = bfs::path(destDir) / (skAsset->name + "_" + std::to_string(n) + ".nurag");
			if (rd->SaveToFile(rout.string()))
			{
				const std::string rpath = rout.string();
				AssImporter::Reg([rd, rpath]
				{
					ResDB::getSingleton()->RegisterRagdoll(rd);
					ResDB::getSingleton()->SetAssetPath(rd->guid, rpath);
				});
				cout << "[Import]	wrote " << rout.filename().string() << " (" << rd->bodies.size()
				     << " bodies, " << rd->joints.size() << " joints)" << endl;
			}
			else { cout << "[Import]	failed to write " << rout.filename().string() << endl; delete rd; }
		}
	}

	// Clip names come from the FILE stem, not the embedded take name: exporters stamp one
	// generic take name into every file, so a whole pack would collide on it.
	std::string firstClipGuid;
	std::string base0 = SafeStem(bfs::path(srcPath).stem().string().c_str());
	for (unsigned int a = 0; a < sc->mNumAnimations; ++a)
	{
		ProgStage("clip " + std::to_string(a + 1) + "/" + std::to_string(sc->mNumAnimations));
		const aiAnimation* an = sc->mAnimations[a];
		const double tps = an->mTicksPerSecond > 0.0 ? an->mTicksPerSecond : 25.0;
		AnimClip* clip = new AnimClip();
		clip->guid = ResDB::NewGuid();
		if (sc->mNumAnimations == 1)
			clip->name = base0;                                       // "idle.fbx" -> "idle"
		else
			clip->name = base0 + "_" + ((an->mName.length > 0) ? SafeStem(an->mName.C_Str())
			                                                   : std::to_string(a));
		clip->duration = an->mDuration / tps;
		clip->skelGuid = skelGuid;   // the file's shared skeleton ("" = unrigged) — retarget key
		clip->channels.resize(an->mNumChannels);
		for (unsigned int c = 0; c < an->mNumChannels; ++c)
		{
			const aiNodeAnim* na = an->mChannels[c];
			AnimClip::Channel& ch = clip->channels[c];
			ch.bone = na->mNodeName.C_Str();
			ch.pos.resize(na->mNumPositionKeys);
			for (unsigned int k = 0; k < na->mNumPositionKeys; ++k)
			{
				ch.pos[k].t = (float)(na->mPositionKeys[k].mTime / tps);
				const aiVector3D& v = na->mPositionKeys[k].mValue;
				ch.pos[k].v[0] = v.x; ch.pos[k].v[1] = v.y; ch.pos[k].v[2] = v.z; ch.pos[k].v[3] = 0;
			}
			ch.rot.resize(na->mNumRotationKeys);
			for (unsigned int k = 0; k < na->mNumRotationKeys; ++k)
			{
				ch.rot[k].t = (float)(na->mRotationKeys[k].mTime / tps);
				const aiQuaternion& q = na->mRotationKeys[k].mValue;
				ch.rot[k].v[0] = q.x; ch.rot[k].v[1] = q.y; ch.rot[k].v[2] = q.z; ch.rot[k].v[3] = q.w;
			}
			ch.scl.resize(na->mNumScalingKeys);
			for (unsigned int k = 0; k < na->mNumScalingKeys; ++k)
			{
				ch.scl[k].t = (float)(na->mScalingKeys[k].mTime / tps);
				const aiVector3D& v = na->mScalingKeys[k].mValue;
				ch.scl[k].v[0] = v.x; ch.scl[k].v[1] = v.y; ch.scl[k].v[2] = v.z; ch.scl[k].v[3] = 0;
			}
		}
		// Keyframe reduction: drop interior keys the neighbours reconstruct within tolerance
		// (exporters bake every frame). Rotations compare all 4 quat components.
		{
			auto reduce = [](std::vector<AnimClip::Key>& keys, int comps, float tol)
			{
				if (keys.size() < 3) return (size_t)0;
				std::vector<AnimClip::Key> out;
				out.reserve(keys.size());
				out.push_back(keys.front());
				for (size_t i = 1; i + 1 < keys.size(); ++i)
				{
					const AnimClip::Key& a = out.back();
					const AnimClip::Key& b = keys[i];
					const AnimClip::Key& c = keys[i + 1];
					const float f = (c.t > a.t) ? (b.t - a.t) / (c.t - a.t) : 0.0f;
					bool keep = false;
					for (int k = 0; k < comps && !keep; ++k)
					{
						const float lerped = a.v[k] + (c.v[k] - a.v[k]) * f;
						if (fabsf(lerped - b.v[k]) > tol) keep = true;
					}
					if (keep) out.push_back(b);
				}
				out.push_back(keys.back());
				const size_t dropped = keys.size() - out.size();
				keys.swap(out);
				return dropped;
			};
			size_t before = 0, dropped = 0;
			for (AnimClip::Channel& ch : clip->channels)
			{
				before += ch.pos.size() + ch.rot.size() + ch.scl.size();
				dropped += reduce(ch.pos, 3, 0.0005f);
				dropped += reduce(ch.rot, 4, 0.0005f);
				dropped += reduce(ch.scl, 3, 0.0005f);
			}
			if (dropped)
				cout << "[Import]\tclip '" << clip->name << "': compressed " << before << " -> "
				     << (before - dropped) << " keys" << endl;
		}
		bfs::path aout = bfs::path(destDir) / (clip->name + ".nuanim");
		for (int nn = 1; bfs::exists(aout, ec); ++nn)
			aout = bfs::path(destDir) / (clip->name + "_" + std::to_string(nn) + ".nuanim");
		if (clip->SaveToFile(aout.string()))
		{
			const std::string apath = aout.string();
			AnimClip* cptr = clip;
			AssImporter::Reg([cptr, apath]
			{
				ResDB::getSingleton()->RegisterClip(cptr);
				ResDB::getSingleton()->SetAssetPath(cptr->guid, apath);
			});   // main-thread when async
			if (firstClipGuid.empty()) firstClipGuid = clip->guid;
			cout << "[Import]\twrote " << aout.filename().string() << " (" << clip->duration << " s, "
			     << clip->channels.size() << " channels)" << endl;
		}
		else { cout << "[Import]\tfailed to write " << aout.filename().string() << endl; delete clip; }
		ProgUnitDone();
	}

	// Prefab only when the file HAS meshes: an animation-only file would otherwise produce
	// a prefab of hundreds of empty joint atoms.
	ProgStage("prefab");
	if (count > 0)
	{
		Atom* root = BuildPrefabNode(sc->mRootNode, sc, nodeMeshes, matGuids, boneNames, firstClipGuid);
		// Skinned file: ONE Animator on the ROOT — it drives every subtree
		// SkinnedMeshRenderer through the shared skeleton.
		if (!skelGuid.empty())
		{
			Animator* an = new Animator();
			an->clipGuid = firstClipGuid;   // "" when the file carries no clips
			root->AddComponent(an);
		}
		std::string base = SafeStem(bfs::path(srcPath).stem().string().c_str());
		bfs::path pf = bfs::path(destDir) / (base + ".nuprefab");
		for (int n = 1; bfs::exists(pf, ec); ++n)
			pf = bfs::path(destDir) / (base + "_" + std::to_string(n) + ".nuprefab");
		if (SavePrefab(root, pf.string()))
			cout << "[Import]\twrote " << pf.filename().string() << endl;
		// root is persisted as the prefab; not added to any world here.
	}
	ProgUnitDone();

	if (count > 0)
		cout << "[Import]\tconverted " << count << " mesh(es) + prefab from "
		     << bfs::path(srcPath).filename().string() << endl;
	else if (sc->mNumAnimations > 0)
		cout << "[Import]\tanimation-only file: " << sc->mNumAnimations << " clip(s), no prefab ("
		     << bfs::path(srcPath).filename().string() << ")" << endl;
	// Clips alone are a SUCCESSFUL import (animation packs).
	return count > 0 ? count : (int)sc->mNumAnimations;
}

std::string AssImporter::ImportImage(const char* srcPath, const char* destDir)
{
	std::string ext = bfs::path(srcPath).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);

	if (tlProg) { tlProg->done = 0; tlProg->total = 1; }   // one unit: this image
	ProgStage("reading");

	Texture* tex = new Texture();
	tex->guid = ResDB::NewGuid();
	tex->usage = (Texture::Usage)Texture::GuessUsage(srcPath);   // bare image: role guessed from the filename suffix
	int w = 0, h = 0;

	if (ext == ".gif")
	{
		// Animated GIF: load ALL frames + per-frame delays (frames stacked w*h*frames*4, RGBA8, no BC/mips).
		bfs::ifstream gf(bfs::path(srcPath), std::ios::binary);
		std::vector<unsigned char> buf((std::istreambuf_iterator<char>(gf)), std::istreambuf_iterator<char>());
		if (buf.empty()) { delete tex; cout << "[Import]\tgif read failed: " << srcPath << endl; return std::string(); }
		int frames = 0, comp = 0; int* delays = nullptr;
		unsigned char* px = stbi_load_gif_from_memory(buf.data(), (int)buf.size(), &delays, &w, &h, &frames, &comp, 4);
		if (!px || frames < 1) { delete tex; cout << "[Import]\tgif decode failed: " << srcPath << endl; return std::string(); }
		// stb returns DELTA frames (transparent where unchanged) — composite forward into full frames.
		const size_t fb = (size_t)w * h * 4;
		for (int k = 1; k < frames; ++k)
		{
			unsigned char* prev = px + (size_t)(k - 1) * fb;
			unsigned char* cur  = px + (size_t)k * fb;
			for (size_t p = 0; p < (size_t)w * h; ++p)
				if (cur[p * 4 + 3] == 0) { cur[p*4] = prev[p*4]; cur[p*4+1] = prev[p*4+1]; cur[p*4+2] = prev[p*4+2]; cur[p*4+3] = prev[p*4+3]; }
		}
		tex->frameCount = frames;
		tex->frameDelaysMs.resize(frames);
		for (int k = 0; k < frames; ++k) tex->frameDelaysMs[k] = (delays && delays[k] > 0) ? delays[k] : 100;
		bool hasA = false;
		for (size_t p = 0; p < (size_t)w * h * frames && !hasA; ++p) if (px[p * 4 + 3] < 255) hasA = true;
		const int gbb = hasA ? 16 : 8, galpha = hasA ? 1 : 0;
		tex->format = hasA ? Texture::FMT_BC3 : Texture::FMT_BC1; tex->mipCount = 1; tex->pixels.clear();
		int pw = 0, ph = 0;
		for (int k = 0; k < frames; ++k)
		{
			ProgStage("compressing frame " + std::to_string(k + 1) + "/" + std::to_string(frames),
			          (float)k / (float)frames);
			std::vector<unsigned char> frame(px + (size_t)k * fb, px + (size_t)(k + 1) * fb);
			std::vector<unsigned char> padded = PadTo4(frame, w, h, pw, ph);
			BCLevel(tex->pixels, padded.data(), pw, ph, gbb, galpha);
		}
		tex->width = pw; tex->height = ph;
		stbi_image_free(px);
		if (delays) STBI_FREE(delays);
	}
	else
	{
		int n = 0;
		unsigned char* px = stbi_load(srcPath, &w, &h, &n, 4);
		if (!px) { delete tex; cout << "[Import]\timage load failed: " << srcPath << endl; return std::string(); }
		std::vector<unsigned char> rgba(px, px + (size_t)w * h * 4);
		stbi_image_free(px);
		CompressToBC(tex, rgba, w, h, tex->usage,
		             [](float f) { ProgStage("compressing", f); });   // BC1/BC3/BC5 + mip chain (static images)
	}

	std::string stem = SafeStem(bfs::path(srcPath).stem().string().c_str());
	boost::system::error_code ec;
	bfs::path out = bfs::path(destDir) / (stem + ".nutex");
	for (int k = 1; bfs::exists(out, ec); ++k)
		out = bfs::path(destDir) / (stem + "_" + std::to_string(k) + ".nutex");
	if (!tex->SaveToFile(out.string())) { delete tex; return std::string(); }
	const std::string outPath = out.string();
	AssImporter::Reg([tex, outPath]
	{
		ResDB::getSingleton()->RegisterTexture(tex);
		ResDB::getSingleton()->SetAssetPath(tex->guid, outPath);
	});   // main-thread when async
	cout << "[Import]\twrote " << out.filename().string() << " (" << w << "x" << h << ")" << endl;
	return tex->guid;
}

// Import an audio file as a collision-safe COPY into content. No custom asset format and
// nothing to register — components reference the file by its content-relative path.
bool AssImporter::ImportAudio(const char* srcPath, const char* destDir)
{
	if (tlProg) { tlProg->done = 0; tlProg->total = 1; }
	ProgStage("copying");
	std::string stem = SafeStem(bfs::path(srcPath).stem().string().c_str());
	std::string ext  = bfs::path(srcPath).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	boost::system::error_code ec;
	bfs::path out = bfs::path(destDir) / (stem + ext);
	for (int k = 1; bfs::exists(out, ec); ++k)
		out = bfs::path(destDir) / (stem + "_" + std::to_string(k) + ext);
	bfs::copy_file(bfs::path(srcPath), out, ec);
	if (ec) { cout << "[Import]\taudio copy failed: " << srcPath << " (" << ec.message() << ")" << endl; return false; }
	cout << "[Import]\twrote " << out.filename().string() << " (audio)" << endl;
	return true;
}

// --- plugin importer registry (interface/Importers.h) --------------------------------
static std::vector<AssetImporter>& importerReg() { static std::vector<AssetImporter> v; return v; }

void RegisterImporter(const AssetImporter& imp)
{
	for (const AssetImporter& e : importerReg())   // dedup by label (re-enabling a plugin re-registers)
		if (e.label == imp.label) return;
	importerReg().push_back(imp);
}
const std::vector<AssetImporter>& AssetImporters() { return importerReg(); }
void ImporterDefer(const std::function<void()>& fn) { AssImporter::Reg(fn); }   // -> main thread (see AssImporter::Reg)
const AssetImporter* ImporterForExt(const std::string& ext)
{
	std::string want = ext;
	for (char& c : want) c = (char)std::tolower((unsigned char)c);
	for (const AssetImporter& e : importerReg())
		for (const std::string& x : e.exts)
		{
			std::string xl = x; for (char& c : xl) c = (char)std::tolower((unsigned char)c);
			if (xl == want) return &e;
		}
	return nullptr;
}

bool AssImporter::ImportAny(const char* srcPath, const char* destDir)
{
	if (!srcPath || !*srcPath) return false;
	std::string ext = bfs::path(srcPath).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	// Plugin importers win over built-ins (a plugin may deliberately override one).
	if (const AssetImporter* imp = ImporterForExt(ext))
		return imp->import ? imp->import(srcPath, destDir) : false;
	static const char* kImg[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic", ".ppm", ".pgm" };
	for (const char* e : kImg)
		if (ext == e) return !ImportImage(srcPath, destDir).empty();
	static const char* kAud[] = { ".ogg", ".wav", ".mp3", ".flac" };
	for (const char* e : kAud)
		if (ext == e) return ImportAudio(srcPath, destDir);
	return ImportToContent(srcPath, destDir) > 0;
}

// Thread-local defer sink: non-null only inside a WORKER import; a synchronous import
// on the game thread applies mutations immediately.
static thread_local std::vector<boost::function<void()>>* tlDeferSink = nullptr;

void AssImporter::Reg(const boost::function<void()>& f)
{
	if (tlDeferSink) tlDeferSink->push_back(f);
	else f();
}

void AssImporter::ImportAnyAsync(const std::string& srcPath, const std::string& destDir,
                                 boost::function<void(bool)> onDone)
{
	// Unique status-bar entry per queued import.
	static boost::atomic<int> seq(0);
	const std::string key  = "import#" + std::to_string(++seq);
	const std::string name = bfs::path(srcPath).filename().string();
	StatusBar::Set(key, name + " — queued", StatusBar::kIndeterminate);

	AssImporter* self = getSingleton();
	Jobs::Schedule([self, srcPath, destDir, onDone, key, name]()
	{
		// Serialize imports: two racing over the same destination names would collide.
		static boost::mutex importLock;
		boost::mutex::scoped_lock l(importLock);

		auto defers = std::make_shared<std::vector<boost::function<void()>>>();
		ImportProgress prog;
		prog.key = key; prog.name = name;
		tlProg = &prog;
		tlDeferSink = defers.get();
		bool ok = false;
		try { ok = self->ImportAny(srcPath.c_str(), destDir.c_str()); }
		catch (const std::exception& e) { cout << "[Import]	async import threw: " << e.what() << endl; }
		tlDeferSink = nullptr;
		tlProg = nullptr;

		// Registrations + completion land on the GAME thread (ResDB is not thread-safe).
		StatusBar::Set(key, name + " — registering", 1.0f);
		Jobs::RunOnMain([defers, onDone, ok, key]()
		{
			for (auto& f : *defers) f();
			StatusBar::Remove(key);
			if (onDone) onDone(ok);
		});
	});
}
}  // namespace nuke
