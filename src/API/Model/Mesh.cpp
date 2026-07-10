#include "API/Model/Mesh.h"
#include <sstream>
#include <assimp/scene.h>
#include <algorithm>
#include <array>
#include <vector>
#include <map>
#include <set>
#include <cmath>
#include <cstdint>
#include <boost/filesystem/fstream.hpp>

namespace nuke { namespace bfs = boost::filesystem; }

namespace nuke {

Mesh::Mesh() {
	vertexArray = nullptr;
	normalArray = nullptr;
	uvArray     = nullptr;
	numVerts    = 0;
	name[0]     = '\0';
	children.clear();
}

// Compute the local-space AABB from vertexArray once (cached). Used for frustum culling.
void Mesh::EnsureBounds() {
	if (boundsValid) return;
	if (!vertexArray || numVerts <= 0) return;
	float mn[3] = { vertexArray[0], vertexArray[1], vertexArray[2] };
	float mx[3] = { mn[0], mn[1], mn[2] };
	for (int i = 0; i < numVerts; ++i)
		for (int c = 0; c < 3; ++c) {
			float v = vertexArray[i * 3 + c];
			if (v < mn[c]) mn[c] = v;
			if (v > mx[c]) mx[c] = v;
		}
	for (int c = 0; c < 3; ++c) { aabbMin[c] = mn[c]; aabbMax[c] = mx[c]; }
	boundsValid = true;
}

// assimp matrices are ROW-major; glm/our storage is COLUMN-major -> transpose on copy.
static void AiToCol16(const aiMatrix4x4& m, float out[16])
{
	const float* s = &m.a1;                       // row-major 4x4
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out[c * 4 + r] = s[r * 4 + c];
}

// Build the skeleton for `mesh`: its bone nodes + every node on the root->bone paths
// (clips animate intermediates too), in DFS pre-order so parent index < child index.
static void BuildSkeleton(aiMesh* mesh, const aiScene* sc, std::vector<MeshBone>& outBones,
                          std::map<std::string, int>& outIndex)
{
	std::set<const aiNode*> needed;
	for (unsigned int b = 0; b < mesh->mNumBones; ++b)
	{
		const aiNode* n = sc->mRootNode->FindNode(mesh->mBones[b]->mName);
		for (; n; n = n->mParent) needed.insert(n);   // the bone + all ancestors
	}
	if (needed.empty()) return;

	struct Walker
	{
		std::set<const aiNode*>& needed;
		std::vector<MeshBone>&   bones;
		std::map<std::string, int>& index;
		void Walk(const aiNode* n, int parent)
		{
			int self = parent;
			if (needed.count(n))
			{
				MeshBone mb;
				mb.name   = n->mName.C_Str();
				mb.parent = parent;
				for (int k = 0; k < 16; ++k) mb.invBind[k] = (k % 5 == 0) ? 1.0f : 0.0f;   // identity until aiBone fills it
				aiVector3D p, s; aiQuaternion r;
				n->mTransformation.Decompose(s, r, p);
				mb.localPos[0] = p.x; mb.localPos[1] = p.y; mb.localPos[2] = p.z;
				mb.localRot[0] = r.x; mb.localRot[1] = r.y; mb.localRot[2] = r.z; mb.localRot[3] = r.w;
				mb.localScale[0] = s.x; mb.localScale[1] = s.y; mb.localScale[2] = s.z;
				self = (int)bones.size();
				index[mb.name] = self;
				bones.push_back(mb);
			}
			for (unsigned int c = 0; c < n->mNumChildren; ++c) Walk(n->mChildren[c], self);
		}
	} w{ needed, outBones, outIndex };
	w.Walk(sc->mRootNode, -1);

	for (unsigned int b = 0; b < mesh->mNumBones; ++b)
	{
		auto it = outIndex.find(mesh->mBones[b]->mName.C_Str());
		if (it != outIndex.end()) AiToCol16(mesh->mBones[b]->mOffsetMatrix, outBones[it->second].invBind);
	}
}

void Mesh::ImportAIMesh(aiMesh* mesh, const aiScene* scene) {
	numVerts = mesh->mNumFaces * 3;

	// --- skin: per-ORIGINAL-vertex weights first (expanded to the soup with the faces) ---
	std::map<std::string, int> boneIdx;
	std::vector<std::array<std::pair<int, float>, 4>> vw;   // per original vertex: up to 4 (bone, weight)
	if (scene && mesh->HasBones())
	{
		BuildSkeleton(mesh, scene, bones, boneIdx);
		if (!bones.empty())
		{
			vw.assign(mesh->mNumVertices, { { { 0, 0.f }, { 0, 0.f }, { 0, 0.f }, { 0, 0.f } } });
			for (unsigned int b = 0; b < mesh->mNumBones; ++b)
			{
				auto bi = boneIdx.find(mesh->mBones[b]->mName.C_Str());
				if (bi == boneIdx.end()) continue;
				for (unsigned int wi = 0; wi < mesh->mBones[b]->mNumWeights; ++wi)
				{
					const aiVertexWeight& aw = mesh->mBones[b]->mWeights[wi];
					if (aw.mVertexId >= vw.size() || aw.mWeight <= 0.0f) continue;
					auto& slots = vw[aw.mVertexId];
					int weakest = 0;
					for (int k = 1; k < 4; ++k) if (slots[k].second < slots[weakest].second) weakest = k;
					if (aw.mWeight > slots[weakest].second) slots[weakest] = { bi->second, aw.mWeight };
				}
			}
			boneIndex  = new unsigned short[(size_t)numVerts * 4];
			boneWeight = new float[(size_t)numVerts * 4];
		}
	}

	vertexArray = new float[mesh->mNumFaces * 3 * 3];
	normalArray = new float[mesh->mNumFaces * 3 * 3];
	uvArray = new float[mesh->mNumFaces * 3 * 2];

	const bool hasUV   = mesh->HasTextureCoords(0);   // models may lack UVs / normals
	const bool hasNorm = mesh->HasNormals();

	for (unsigned int i = 0; i < mesh->mNumFaces; i++)
	{
		const aiFace& face = mesh->mFaces[i];

		for (int j = 0; j < 3; j++)
		{
			unsigned int idx = face.mIndices[j];

			if (hasUV) { aiVector3D uv = mesh->mTextureCoords[0][idx]; memcpy(uvArray, &uv, sizeof(float) * 2); }
			else       { uvArray[0] = 0.0f; uvArray[1] = 0.0f; }
			uvArray += 2;

			if (hasNorm) { aiVector3D normal = mesh->mNormals[idx]; memcpy(normalArray, &normal, sizeof(float) * 3); }
			else         { normalArray[0] = 0.0f; normalArray[1] = 0.0f; normalArray[2] = 0.0f; }
			normalArray += 3;

			aiVector3D pos = mesh->mVertices[idx];
			memcpy(vertexArray, &pos, sizeof(float) * 3);
			vertexArray += 3;

			if (boneIndex)   // expand this original vertex's (normalized) bindings into the soup slot
			{
				const size_t slot = ((size_t)i * 3 + j) * 4;
				const auto& sw = vw[idx];
				float sum = sw[0].second + sw[1].second + sw[2].second + sw[3].second;
				if (sum <= 0.0f) sum = 1.0f;
				for (int k = 0; k < 4; ++k)
				{
					boneIndex[slot + k]  = (unsigned short)std::min(sw[k].first, 65535);
					boneWeight[slot + k] = sw[k].second / sum;
				}
			}
		}
	}

	uvArray -= mesh->mNumFaces * 3 * 2;
	normalArray -= mesh->mNumFaces * 3 * 3;
	vertexArray -= mesh->mNumFaces * 3 * 3;
	const char* __name = mesh->mName.C_Str();
	strcpy(name, __name);
	//name = __name;
}

Mesh* Mesh::CreateCube() {
	Mesh* m = new Mesh();
	const float h = 0.5f;
	const float C[8][3] = {
		{-h,-h,-h},{ h,-h,-h},{ h, h,-h},{-h, h,-h},
		{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}
	};
	const int   F[6][4] = { {4,5,6,7},{1,0,3,2},{5,1,2,6},{0,4,7,3},{7,6,2,3},{0,1,5,4} };
	const float N[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
	const int   tri[6]  = { 0,1,2, 0,2,3 };
	const float UV[4][2] = { {1,1}, {0,1}, {0,0}, {1,0} };   // per-face corner UVs (U flipped: no mirroring)

	m->numVerts   = 36;
	m->vertexArray = new float[36 * 3];
	m->normalArray = new float[36 * 3];
	m->uvArray     = new float[36 * 2];
	int vi = 0;
	for (int f = 0; f < 6; ++f)
		for (int t = 0; t < 6; ++t)
		{
			const float* p = C[F[f][tri[t]]];
			m->vertexArray[vi * 3 + 0] = p[0]; m->vertexArray[vi * 3 + 1] = p[1]; m->vertexArray[vi * 3 + 2] = p[2];
			m->normalArray[vi * 3 + 0] = N[f][0]; m->normalArray[vi * 3 + 1] = N[f][1]; m->normalArray[vi * 3 + 2] = N[f][2];
			m->uvArray[vi * 2 + 0] = UV[tri[t]][0]; m->uvArray[vi * 2 + 1] = UV[tri[t]][1];
			++vi;
		}
	strcpy(m->name, "Cube");
	return m;
}

Mesh* Mesh::CreatePlane() {
	Mesh* m = new Mesh();
	const float h = 0.5f;
	const float P[6][3] = {
		{-h,0,-h},{-h,0, h},{ h,0, h},
		{-h,0,-h},{ h,0, h},{ h,0,-h}
	};
	m->numVerts    = 6;
	m->vertexArray = new float[6 * 3];
	m->normalArray = new float[6 * 3];
	m->uvArray     = new float[6 * 2]();
	for (int i = 0; i < 6; ++i)
	{
		m->vertexArray[i*3+0] = P[i][0]; m->vertexArray[i*3+1] = P[i][1]; m->vertexArray[i*3+2] = P[i][2];
		m->normalArray[i*3+0] = 0; m->normalArray[i*3+1] = 1; m->normalArray[i*3+2] = 0;
		m->uvArray[i*2+0] = h - P[i][0]; m->uvArray[i*2+1] = P[i][2] + h;   // [-h,h] -> [0,1] (U flipped)
	}
	strcpy(m->name, "Plane");
	return m;
}

Mesh* Mesh::CreateSphere() {
	Mesh* m = new Mesh();
	const int   ST = 16, SE = 24;     // stacks, sectors
	const float R = 0.5f;
	const float PI = 3.14159265358979f;
	std::vector<float> v, n, uv;
	auto at = [&](int st, int se, float out[3]) {
		float phi = PI * (float)st / ST;
		float th  = 2.0f * PI * (float)se / SE;
		out[0] = R * sinf(phi) * cosf(th);
		out[1] = R * cosf(phi);
		out[2] = R * sinf(phi) * sinf(th);
	};
	auto push = [&](const float p[3], int st, int se) {
		v.push_back(p[0]); v.push_back(p[1]); v.push_back(p[2]);
		float l = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]); if (l < 1e-6f) l = 1.0f;
		n.push_back(p[0]/l); n.push_back(p[1]/l); n.push_back(p[2]/l);
		uv.push_back(1.0f - (float)se / SE); uv.push_back((float)st / ST);   // equirectangular (U flipped)
	};
	for (int st = 0; st < ST; ++st)
		for (int se = 0; se < SE; ++se)
		{
			float a[3], b[3], c[3], d[3];
			at(st, se, a); at(st+1, se, b); at(st+1, se+1, c); at(st, se+1, d);
			push(a, st, se); push(b, st+1, se); push(c, st+1, se+1);   // tri 1
			push(a, st, se); push(c, st+1, se+1); push(d, st, se+1);   // tri 2
		}
	m->numVerts    = (int)(v.size() / 3);
	m->vertexArray = new float[v.size()];  memcpy(m->vertexArray, v.data(), v.size() * sizeof(float));
	m->normalArray = new float[n.size()];  memcpy(m->normalArray, n.data(), n.size() * sizeof(float));
	m->uvArray     = new float[uv.size()]; memcpy(m->uvArray, uv.data(), uv.size() * sizeof(float));
	strcpy(m->name, "Sphere");
	return m;
}

// ---- native .numesh (binary) -------------------------------------------------
// Layout: magic "NUMESH\0\0" | u32 version | u32 nameLen + name | u32 guidLen + guid |
//         i32 numVerts | f32 pos[3N] | u8 hasNormals (+ f32 nrm[3N]) | u8 hasUV (+ f32 uv[2N])
// v2 appends the SKIN block: u8 hasSkin { u32 boneCount, per bone: str name | i32 parent |
//         f32 invBind[16] | f32 localPos[3] | f32 localRot[4] | f32 localScale[3];
//         boneIndex[4N] | f32 boneWeight[4N] }.  v1 files load with no skin.
// v3: boneIndex widened u8 -> u16 (FBX helper-node skeletons exceed 255 joints).
namespace {
	const char  kMagic[8] = { 'N','U','M','E','S','H','\0','\0' };
	const uint32_t kVersion = 3;
	template <class T> void wr(bfs::ofstream& o, const T& v) { o.write((const char*)&v, sizeof(T)); }
	template <class T> void rd(std::istream& i, T& v)       { i.read((char*)&v, sizeof(T)); }
	void wrStr(bfs::ofstream& o, const std::string& s) { uint32_t n = (uint32_t)s.size(); wr(o, n); if (n) o.write(s.data(), n); }
	std::string rdStr(std::istream& i) { uint32_t n = 0; rd(i, n); std::string s(n, '\0'); if (n) i.read(&s[0], n); return s; }
}

bool Mesh::SaveToFile(const std::string& path) const
{
	bfs::ofstream o(bfs::path(path), std::ios::binary);
	if (!o) return false;
	o.write(kMagic, 8);
	wr(o, kVersion);
	wrStr(o, std::string(name));
	wrStr(o, guid);
	int32_t n = numVerts;
	wr(o, n);
	if (n > 0 && vertexArray) o.write((const char*)vertexArray, sizeof(float) * 3 * n);
	uint8_t hasN = (normalArray != nullptr) ? 1 : 0; wr(o, hasN);
	if (hasN) o.write((const char*)normalArray, sizeof(float) * 3 * n);
	uint8_t hasUV = (uvArray != nullptr) ? 1 : 0; wr(o, hasUV);
	if (hasUV) o.write((const char*)uvArray, sizeof(float) * 2 * n);
	uint8_t hasSkin = HasSkin() ? 1 : 0; wr(o, hasSkin);
	if (hasSkin)
	{
		uint32_t bc = (uint32_t)bones.size(); wr(o, bc);
		for (const MeshBone& b : bones)
		{
			wrStr(o, b.name);
			int32_t par = b.parent; wr(o, par);
			o.write((const char*)b.invBind,    sizeof(float) * 16);
			o.write((const char*)b.localPos,   sizeof(float) * 3);
			o.write((const char*)b.localRot,   sizeof(float) * 4);
			o.write((const char*)b.localScale, sizeof(float) * 3);
		}
		o.write((const char*)boneIndex,  sizeof(unsigned short) * 4 * n);
		o.write((const char*)boneWeight, sizeof(float) * 4 * n);
	}
	return (bool)o;
}

Mesh* Mesh::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	return LoadFromStream(i);
}

Mesh* Mesh::LoadFromMemory(const std::string& data)
{
	std::istringstream i(data, std::ios::binary);
	return LoadFromStream(i);
}

Mesh* Mesh::LoadFromStream(std::istream& i)
{
	char magic[8]; i.read(magic, 8);
	if (memcmp(magic, kMagic, 8) != 0) return nullptr;
	uint32_t version = 0; rd(i, version);
	(void)version;
	Mesh* m = new Mesh();
	std::string nm = rdStr(i);
	strncpy(m->name, nm.c_str(), sizeof(m->name) - 1); m->name[sizeof(m->name) - 1] = 0;
	m->guid = rdStr(i);
	int32_t n = 0; rd(i, n);
	m->numVerts = n;
	if (n > 0)
	{
		m->vertexArray = new float[3 * n];
		i.read((char*)m->vertexArray, sizeof(float) * 3 * n);
	}
	uint8_t hasN = 0; rd(i, hasN);
	if (hasN && n > 0) { m->normalArray = new float[3 * n]; i.read((char*)m->normalArray, sizeof(float) * 3 * n); }
	uint8_t hasUV = 0; rd(i, hasUV);
	if (hasUV && n > 0) { m->uvArray = new float[2 * n]; i.read((char*)m->uvArray, sizeof(float) * 2 * n); }
	if (version >= 2)   // skin block (v1 files simply have none)
	{
		uint8_t hasSkin = 0; rd(i, hasSkin);
		if (hasSkin && n > 0)
		{
			uint32_t bc = 0; rd(i, bc);
			m->bones.resize(bc);
			for (uint32_t b = 0; b < bc; ++b)
			{
				MeshBone& mb = m->bones[b];
				mb.name = rdStr(i);
				int32_t par = -1; rd(i, par); mb.parent = par;
				i.read((char*)mb.invBind,    sizeof(float) * 16);
				i.read((char*)mb.localPos,   sizeof(float) * 3);
				i.read((char*)mb.localRot,   sizeof(float) * 4);
				i.read((char*)mb.localScale, sizeof(float) * 3);
			}
			m->boneIndex  = new unsigned short[(size_t)n * 4];
			m->boneWeight = new float[(size_t)n * 4];
			if (version >= 3)
				i.read((char*)m->boneIndex, sizeof(unsigned short) * 4 * n);
			else   // v2 stored u8 indices — widen on load
			{
				std::vector<unsigned char> old((size_t)n * 4);
				i.read((char*)old.data(), old.size());
				for (size_t k = 0; k < old.size(); ++k) m->boneIndex[k] = old[k];
			}
			i.read((char*)m->boneWeight, sizeof(float) * 4 * n);
		}
	}
	if (!i && !i.eof()) { delete m; return nullptr; }
	return m;
}
}  // namespace nuke