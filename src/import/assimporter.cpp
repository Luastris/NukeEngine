#include "import/assimporter.h"
#include "API/Model/Prefab.h"
#include "API/Model/Transform.h"
#include "API/Model/Texture.h"
#include <boost/filesystem.hpp>
#include <vector>
#include <string>
#include <map>
#include <cctype>
#include <cstdlib>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace nuke {

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
// MeshRenderer (referencing the converted mesh by GUID) per mesh on the node.
static Atom* BuildPrefabNode(aiNode* node, const aiScene* sc,
                             const std::vector<std::string>& meshGuids,
                             const std::vector<std::string>& matGuids)
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
		go->AddChild(child);
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i)
		go->AddChild(BuildPrefabNode(node->mChildren[i], sc, meshGuids, matGuids));
	return go;
}

// Decode a texture (external file, or assimp-embedded "*N") to RGBA8 and write a .nutex asset.
// Returns the new texture GUID, or "" if it couldn't be loaded. Dedups within one import.
static std::string ConvertTexture(const aiScene* sc, const std::string& texRef,
                                  const bfs::path& modelDir, const std::string& destDir,
                                  std::map<std::string, std::string>& cache)
{
	if (texRef.empty()) return std::string();
	auto cit = cache.find(texRef);
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
	tex->width  = w; tex->height = h;
	tex->pixels = std::move(rgba);

	std::string stem = SafeStem(bfs::path(texRef).stem().string().c_str());
	boost::system::error_code ec;
	bfs::path out = bfs::path(destDir) / (stem + ".nutex");
	for (int k = 1; bfs::exists(out, ec); ++k)
		out = bfs::path(destDir) / (stem + "_" + std::to_string(k) + ".nutex");

	if (!tex->SaveToFile(out.string())) { delete tex; return std::string(); }
	ResDB::getSingleton()->RegisterTexture(tex);
	cache[texRef] = tex->guid;
	cout << "[Import]\twrote " << out.filename().string() << " (" << w << "x" << h << ")" << endl;
	return tex->guid;
}

int AssImporter::ImportToContent(const char* srcPath, const char* destDir)
{
	Assimp::Importer importer;
	const aiScene* sc = importer.ReadFile(srcPath, aiProcessPreset_TargetRealtime_MaxQuality);
	if (!sc)
	{
		cout << "[Import]\t" << importer.GetErrorString() << endl;
		return 0;
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
		aiMaterial* am = sc->mMaterials[i];
		Material* mt = new Material();
		mt->ImportAiMaterial(am);
		mt->guid = ResDB::NewGuid();

		aiString tp;
		if (am->GetTexture(aiTextureType_DIFFUSE, 0, &tp) == AI_SUCCESS)
			mt->diffuseGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache);
		tp.Clear();
		if (am->GetTexture(aiTextureType_NORMALS, 0, &tp) == AI_SUCCESS)
			mt->normalGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache);
		tp.Clear();
		if (am->GetTexture(aiTextureType_SPECULAR, 0, &tp) == AI_SUCCESS)
			mt->specularGuid = ConvertTexture(sc, tp.C_Str(), modelDir, destDir, texCache);
		std::string mstem = SafeStem(mt->matName.empty() ? "material" : mt->matName.c_str());
		bfs::path mout = bfs::path(destDir) / (mstem + ".numat");
		for (int n = 1; bfs::exists(mout, ec); ++n)
			mout = bfs::path(destDir) / (mstem + "_" + std::to_string(n) + ".numat");
		if (mt->SaveToFile(mout.string()))
		{
			res->RegisterMaterial(mt);
			matGuids[i] = mt->guid;
			cout << "[Import]\twrote " << mout.filename().string() << " (" << mt->guid << ")" << endl;
		}
		else { cout << "[Import]\tfailed to write " << mout.filename().string() << endl; delete mt; }
	}

	// 1) Every mesh -> a .numesh asset; remember its GUID per mesh index.
	std::vector<std::string> meshGuids(sc->mNumMeshes);
	int count = 0;
	for (unsigned int i = 0; i < sc->mNumMeshes; ++i)
	{
		Mesh* m = new Mesh();
		m->ImportAIMesh(sc->mMeshes[i]);
		m->guid = ResDB::NewGuid();

		std::string stem = SafeStem(sc->mMeshes[i]->mName.C_Str());
		bfs::path out = bfs::path(destDir) / (stem + ".numesh");
		for (int n = 1; bfs::exists(out, ec); ++n)
			out = bfs::path(destDir) / (stem + "_" + std::to_string(n) + ".numesh");

		if (!m->SaveToFile(out.string()))
		{
			cout << "[Import]\tfailed to write " << out.filename().string() << endl;
			delete m;
			continue;
		}
		res->RegisterMesh(m);          // available immediately (no rescan)
		meshGuids[i] = m->guid;
		cout << "[Import]\twrote " << out.filename().string() << " (" << m->guid << ")" << endl;
		++count;
	}

	// 2) The node hierarchy -> one .nuprefab that references those meshes + materials by GUID.
	Atom* root = BuildPrefabNode(sc->mRootNode, sc, meshGuids, matGuids);
	std::string base = SafeStem(bfs::path(srcPath).stem().string().c_str());
	bfs::path pf = bfs::path(destDir) / (base + ".nuprefab");
	for (int n = 1; bfs::exists(pf, ec); ++n)
		pf = bfs::path(destDir) / (base + "_" + std::to_string(n) + ".nuprefab");
	if (SavePrefab(root, pf.string()))
		cout << "[Import]\twrote " << pf.filename().string() << endl;
	// root is persisted as the prefab; not added to any world here.

	cout << "[Import]\tconverted " << count << " mesh(es) + prefab from "
	     << bfs::path(srcPath).filename().string() << endl;
	return count;
}
}  // namespace nuke