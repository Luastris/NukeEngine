#include "import/assimporter.h"
#include "API/Model/Prefab.h"
#include "API/Model/Transform.h"
#include "API/Model/Texture.h"
#include "API/Model/AnimClip.h"
#include "API/Model/Animator.h"
#include <boost/filesystem.hpp>
#include <vector>
#include <string>
#include <map>
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

// --- live import progress (worker -> status bar) ----------------------------------------
// Only an ASYNC import reports: ImportAnyAsync installs this thread-local sink with its
// status-bar entry key; the conversion stages feed it. Work units = every texture
// conversion + every material + every mesh + the prefab, so the fraction is honest for
// any model. A synchronous game-thread import leaves it null — no bar entry, no cost.
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

// Compress an RGBA image into a BC texture + precomputed mip chain. BC1 (opaque) / BC3 (alpha). ANY size:
// non-multiple-of-4 images are edge-padded to a multiple of 4 (BC needs it) instead of stored raw RGBA.
// BC5 (2-channel R,G) block level — for normal maps. Each 4x4 block = a BC4 block of R then a BC4 block of G (16B).
// The blue channel is dropped (z is reconstructed in the shader as sqrt(1-x^2-y^2)) — proper for tangent normals.
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

// `prog` (optional) gets the compression fraction [0..1], weighted by each mip's pixel
// count — level 0 is ~3/4 of the real work, so the bar moves honestly, not per-level.
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
				for (int c = 0; c < 4; ++c)
				{
					int a = cur[((size_t)y0 * w + x0) * 4 + c], b = cur[((size_t)y0 * w + x1) * 4 + c];
					int e = cur[((size_t)y1 * w + x0) * 4 + c], f = cur[((size_t)y1 * w + x1) * 4 + c];
					nx[((size_t)y * nw + x) * 4 + c] = (unsigned char)((a + b + e + f) / 4);
				}
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
	auto go = new Atom(node->mName.C_Str());
	for (int i = 0; i < node->mNumMeshes; i++) {
		auto cmesh = node->mMeshes[i];
		Atom* ngo = new Atom(scene->mMeshes[cmesh]->mName.C_Str());
		Mesh* m = new Mesh();
		m->ImportAIMesh(scene->mMeshes[cmesh]);
		res->meshes.push_back(m);

		Material* mat = new Material();
		mat->ImportAiMaterial(scene->mMaterials[scene->mMeshes[cmesh]->mMaterialIndex]);
		ResDB::getSingleton()->materials.push_back(mat);

		MeshRenderer* mr = new MeshRenderer();
		mr->mesh = m;
		mr->mat = mat->Clone();   // owned instance; the asset `mat` stays the shared template in ResDB
		ngo->AddComponent(mr);
		go->AddChild(ngo);
	}
	for (int i = 0; i < node->mNumChildren; i++) {
		go->AddChild(ImportObject(node->mChildren[i], scene));
	}
	return go;
}

void AssImporter::Import(const char* path) {

	Assimp::Importer importer;
	ResDB* res = ResDB::getSingleton();
	const aiScene* sc = importer.ReadFile(path, aiProcessPreset_TargetRealtime_MaxQuality);
	if (!sc)
	{
		cout << importer.GetErrorString() << endl;
		return;
	}

	// ================================================================

	std::cout << sc->HasAnimations() << " " << sc->mNumAnimations << std::endl;
	std::cout << sc->HasCameras() << " " << sc->mNumCameras << std::endl;
	std::cout << sc->HasLights() << " " << sc->mNumLights << std::endl;
	std::cout << sc->HasMaterials() << " " << sc->mNumMaterials << std::endl;
	std::cout << sc->HasMeshes() << " " << sc->mNumMeshes << std::endl;
	std::cout << sc->HasTextures() << " " << sc->mNumTextures << std::endl;

	// ================================================================

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

// Rebuild the assimp node tree as an Atom hierarchy, with each node's transform and a
// MeshRenderer (referencing the converted mesh by GUID) per mesh on the node. Skinned
// meshes also get an Animator wired to the model's first imported clip (3.1).
static Atom* BuildPrefabNode(aiNode* node, const aiScene* sc,
                             const std::vector<std::string>& meshGuids,
                             const std::vector<std::string>& matGuids,
                             const std::string& firstClipGuid = std::string())
{
	Atom* go = new Atom(node->mName.C_Str());

	aiVector3D pos, scl; aiQuaternion rot;
	node->mTransformation.Decompose(scl, rot, pos);
	Transform& t = go->GetTransform();
	t.position.x = pos.x; t.position.y = pos.y; t.position.z = pos.z;
	t.rotation.x = rot.x; t.rotation.y = rot.y; t.rotation.z = rot.z; t.rotation.w = rot.w;
	t.scale.x    = scl.x; t.scale.y    = scl.y; t.scale.z    = scl.z;

	for (unsigned int i = 0; i < node->mNumMeshes; ++i)
	{
		unsigned int mi = node->mMeshes[i];
		Atom* child = new Atom(sc->mMeshes[mi]->mName.C_Str());
		MeshRenderer* mr = new MeshRenderer();
		mr->meshGuid = (mi < meshGuids.size()) ? meshGuids[mi] : std::string();
		unsigned int matIdx = sc->mMeshes[mi]->mMaterialIndex;
		if (matIdx < matGuids.size()) mr->matGuid = matGuids[matIdx];
		child->AddComponent(mr);
		if (sc->mMeshes[mi]->HasBones())   // skinned: ready-to-use Animator (clip may be picked later)
		{
			Animator* an = new Animator();
			an->clipGuid = firstClipGuid;   // "" when the file carries no clips
			child->AddComponent(an);
		}
		go->AddChild(child);
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i)
		go->AddChild(BuildPrefabNode(node->mChildren[i], sc, meshGuids, matGuids, firstClipGuid));
	return go;
}

// Decode a texture (external file, or assimp-embedded "*N") to RGBA8 and write a .nutex asset.
// Returns the new texture GUID, or "" if it couldn't be loaded. Dedups within one import.
static std::string ConvertTexture(const aiScene* sc, const std::string& texRef,
                                  const bfs::path& modelDir, const std::string& destDir,
                                  std::map<std::string, std::string>& cache, int usage = Texture::UsageColor)
{
	if (texRef.empty()) return std::string();

	// One progress unit per call, WHATEVER the exit path (cache hits and failures were
	// counted into the total too — see the pre-scan in ImportToContent).
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
	tex->usage  = usage;             // authoritative from the assimp texture type
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
	// COLLAPSE FBX transform pivots: without this assimp splits every FBX node into
	// $AssimpFbx$_Translation/PreRotation/... pseudo-nodes — skeletons balloon (mixamo:
	// 65 joints -> 700 nodes) and, worse, animation channels land on pseudo-node names
	// that DIFFER between files, so a clip from one FBX never binds to a skeleton
	// imported from another. Collapsed, channels and bones use the REAL joint names.
	importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
	const aiScene* sc = importer.ReadFile(srcPath, aiProcessPreset_TargetRealtime_MaxQuality);
	if (!sc)
	{
		cout << "[Import]\t" << importer.GetErrorString() << endl;
		return 0;
	}

	// Progress total: mirror the conversion loops below — one unit per texture slot that
	// WILL be converted (non-empty ref), per material, per mesh, plus the prefab.
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
		tlProg->done  = 0;
		tlProg->total = texUnits + (int)sc->mNumMaterials + (int)sc->mNumMeshes
		              + (int)sc->mNumAnimations + 1;
	}

	boost::system::error_code ec;
	bfs::create_directories(destDir, ec);
	ResDB* res = ResDB::getSingleton();

	// 0) Every material -> a .numat asset (+ its textures -> .nutex); remember GUID per index.
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
		// PBR maps (metallic-roughness, occlusion, emissive).
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

	// 1) Every mesh -> a .numesh asset; remember its GUID per mesh index.
	std::vector<std::string> meshGuids(sc->mNumMeshes);
	int count = 0;
	for (unsigned int i = 0; i < sc->mNumMeshes; ++i)
	{
		ProgStage("mesh " + std::to_string(i + 1) + "/" + std::to_string(sc->mNumMeshes));
		Mesh* m = new Mesh();
		m->ImportAIMesh(sc->mMeshes[i], sc);   // scene enables skin import (bones + skeleton)
		m->guid = ResDB::NewGuid();

		std::string stem = SafeStem(sc->mMeshes[i]->mName.C_Str());
		bfs::path out = bfs::path(destDir) / (stem + ".numesh");
		for (int n = 1; bfs::exists(out, ec); ++n)
			out = bfs::path(destDir) / (stem + "_" + std::to_string(n) + ".numesh");

		if (!m->SaveToFile(out.string()))
		{
			cout << "[Import]\tfailed to write " << out.filename().string() << endl;
			delete m;
			ProgUnitDone();
			continue;
		}
		const std::string mpath = out.string();
		AssImporter::Reg([m, mpath]
		{
			ResDB::getSingleton()->RegisterMesh(m);
			ResDB::getSingleton()->SetAssetPath(m->guid, mpath);
		});   // main-thread when async
		meshGuids[i] = m->guid;
		cout << "[Import]\twrote " << out.filename().string() << " (" << m->guid << ")" << endl;
		++count;
		ProgUnitDone();
	}

	// 2) Animation clips -> .nuanim assets (channels by bone NAME, key times in seconds).
	// Clip names come from the FILE stem, not the embedded take name: exporters stamp a
	// generic name ("mixamo.com") into EVERY file, so a pack would collide on it, while
	// the files are named by the actual animation ("idle.fbx", "run.fbx").
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

	// 3) The node hierarchy -> one .nuprefab that references those meshes + materials by
	// GUID — only when the file HAS meshes. An animation-only file (mixamo "without skin"
	// packs) would otherwise produce a prefab of hundreds of EMPTY joint atoms.
	ProgStage("prefab");
	if (count > 0)
	{
		Atom* root = BuildPrefabNode(sc->mRootNode, sc, meshGuids, matGuids, firstClipGuid);
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
	tex->usage = Texture::GuessUsage(srcPath);   // bare image: guess role from filename suffix (overridable in inspector)
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
		// stb returns DELTA frames (transparent where a frame didn't change). Composite forward so every
		// frame is a full image (a transparent pixel inherits the previous composited frame), then make
		// fully opaque — otherwise frames 1..N render mostly empty.
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
		// Keep real transparency: BC3 if ANY frame has alpha < 255 (animated sprites with holes), else BC1.
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

// Audio has NO custom asset format (the audio service decodes the file itself) — import
// is a plain collision-safe COPY into content. Components reference the file by its
// content-relative path, so nothing needs registering.
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

bool AssImporter::ImportAny(const char* srcPath, const char* destDir)
{
	if (!srcPath || !*srcPath) return false;
	std::string ext = bfs::path(srcPath).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	static const char* kImg[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic", ".ppm", ".pgm" };
	for (const char* e : kImg)
		if (ext == e) return !ImportImage(srcPath, destDir).empty();
	static const char* kAud[] = { ".ogg", ".wav", ".mp3", ".flac" };
	for (const char* e : kAud)
		if (ext == e) return ImportAudio(srcPath, destDir);
	return ImportToContent(srcPath, destDir) > 0;
}

// --- async import (2.4) --------------------------------------------------------------

// Thread-local defer sink: non-null only inside a WORKER import; a synchronous import
// running on the game thread applies mutations immediately.
static thread_local std::vector<boost::function<void()>>* tlDeferSink = nullptr;

void AssImporter::Reg(const boost::function<void()>& f)
{
	if (tlDeferSink) tlDeferSink->push_back(f);
	else f();
}

void AssImporter::ImportAnyAsync(const std::string& srcPath, const std::string& destDir,
                                 boost::function<void(bool)> onDone)
{
	// Unique status-bar entry per queued import: several drops show up as several jobs
	// (queued ones sit at "queued" until the serialize lock lets them run).
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
