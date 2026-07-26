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

// A helper shared by cylinder/capsule generation: append one triangle-souped quad (a,b,c,d)
// as two tris (a,b,c)(a,c,d) — the SAME winding CreateSphere uses (a=upper, b=lower, c=lower
// next, d=upper next), so all built-in primitives face out consistently.
namespace {
struct PV { float p[3], n[3], uv[2]; };
inline void PushQuad(std::vector<float>& v, std::vector<float>& n, std::vector<float>& uv,
                     const PV& a, const PV& b, const PV& c, const PV& d)
{
	const PV* order[6] = { &a, &b, &c, &a, &c, &d };
	for (const PV* q : order)
	{
		v.push_back(q->p[0]);  v.push_back(q->p[1]);  v.push_back(q->p[2]);
		n.push_back(q->n[0]);  n.push_back(q->n[1]);  n.push_back(q->n[2]);
		uv.push_back(q->uv[0]); uv.push_back(q->uv[1]);
	}
}
}  // namespace

Mesh* Mesh::CreateCylinder() {
	Mesh* m = new Mesh();
	const int   SE = 24;              // radial sectors
	const float R = 0.5f, h = 0.5f;   // radius, half-height (unit: Ø1, height 1)
	const float PI = 3.14159265358979f;
	std::vector<float> v, n, uv;
	for (int se = 0; se < SE; ++se)
	{
		float t0 = 2*PI*se/SE, t1 = 2*PI*(se+1)/SE;
		float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
		float u0 = (float)se/SE, u1 = (float)(se+1)/SE;
		// side wall (radial normals)
		PV a{{R*c0, h,R*s0},{c0,0,s0},{u0,1}}, b{{R*c0,-h,R*s0},{c0,0,s0},{u0,0}};
		PV c{{R*c1,-h,R*s1},{c1,0,s1},{u1,0}}, d{{R*c1, h,R*s1},{c1,0,s1},{u1,1}};
		PushQuad(v, n, uv, a, b, c, d);
		// top cap (+Y): center, rim@se, rim@se+1 (sphere north-pole winding)
		PV tc{{0, h,0},{0,1,0},{0.5f,0.5f}};
		PV tr0{{R*c0, h,R*s0},{0,1,0},{0.5f+0.5f*c0,0.5f+0.5f*s0}};
		PV tr1{{R*c1, h,R*s1},{0,1,0},{0.5f+0.5f*c1,0.5f+0.5f*s1}};
		PushQuad(v, n, uv, tc, tr0, tr1, tr1);   // d==c => second tri degenerates, one triangle kept
		// bottom cap (-Y): rim@se, center, rim@se+1 (sphere south-pole winding)
		PV bc{{0,-h,0},{0,-1,0},{0.5f,0.5f}};
		PV br0{{R*c0,-h,R*s0},{0,-1,0},{0.5f+0.5f*c0,0.5f+0.5f*s0}};
		PV br1{{R*c1,-h,R*s1},{0,-1,0},{0.5f+0.5f*c1,0.5f+0.5f*s1}};
		PushQuad(v, n, uv, br0, bc, br1, br1);
	}
	m->numVerts    = (int)(v.size() / 3);
	m->vertexArray = new float[v.size()];  memcpy(m->vertexArray, v.data(), v.size() * sizeof(float));
	m->normalArray = new float[n.size()];  memcpy(m->normalArray, n.data(), n.size() * sizeof(float));
	m->uvArray     = new float[uv.size()]; memcpy(m->uvArray, uv.data(), uv.size() * sizeof(float));
	strcpy(m->name, "Cylinder");
	return m;
}

Mesh* Mesh::CreateCapsule() {
	Mesh* m = new Mesh();
	const int   SE = 24, ST = 8;      // radial sectors, stacks per hemisphere
	const float R = 0.5f, ch = 0.5f;  // radius, cylinder HALF-height (total height = 2*ch + 2*R = 2)
	const float PI = 3.14159265358979f;
	std::vector<float> v, n, uv;
	// a point on a hemisphere of radius R centred at (0,yc,0): dir = (sinφcosθ, cosφ, sinφsinθ),
	// pos = centre + R*dir, normal = dir.
	auto vert = [&](float phi, float th, float yc, float u, float vv) -> PV {
		float d[3] = { sinf(phi)*cosf(th), cosf(phi), sinf(phi)*sinf(th) };
		return PV{{R*d[0], yc + R*d[1], R*d[2]}, {d[0],d[1],d[2]}, {u,vv}};
	};
	for (int se = 0; se < SE; ++se)
	{
		float t0 = 2*PI*se/SE, t1 = 2*PI*(se+1)/SE;
		float u0 = (float)se/SE, u1 = (float)(se+1)/SE;
		float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
		// top hemisphere: φ 0..π/2, centre +ch
		for (int st = 0; st < ST; ++st)
		{
			float p0 = (PI*0.5f)*st/ST, p1 = (PI*0.5f)*(st+1)/ST;
			float v0 = 1.0f - 0.1f*st/ST, v1 = 1.0f - 0.1f*(st+1)/ST;
			PushQuad(v, n, uv, vert(p0,t0,ch,u0,v0), vert(p1,t0,ch,u0,v1),
			                   vert(p1,t1,ch,u1,v1), vert(p0,t1,ch,u1,v0));
		}
		// middle cylinder wall: y +ch..-ch, radial normals
		PV a{{R*c0, ch,R*s0},{c0,0,s0},{u0,0.9f}}, b{{R*c0,-ch,R*s0},{c0,0,s0},{u0,0.1f}};
		PV c{{R*c1,-ch,R*s1},{c1,0,s1},{u1,0.1f}}, d{{R*c1, ch,R*s1},{c1,0,s1},{u1,0.9f}};
		PushQuad(v, n, uv, a, b, c, d);
		// bottom hemisphere: φ π/2..π, centre -ch
		for (int st = 0; st < ST; ++st)
		{
			float p0 = PI*0.5f + (PI*0.5f)*st/ST, p1 = PI*0.5f + (PI*0.5f)*(st+1)/ST;
			float v0 = 0.1f - 0.1f*st/ST, v1 = 0.1f - 0.1f*(st+1)/ST;
			PushQuad(v, n, uv, vert(p0,t0,-ch,u0,v0), vert(p1,t0,-ch,u0,v1),
			                   vert(p1,t1,-ch,u1,v1), vert(p0,t1,-ch,u1,v0));
		}
	}
	m->numVerts    = (int)(v.size() / 3);
	m->vertexArray = new float[v.size()];  memcpy(m->vertexArray, v.data(), v.size() * sizeof(float));
	m->normalArray = new float[n.size()];  memcpy(m->normalArray, n.data(), n.size() * sizeof(float));
	m->uvArray     = new float[uv.size()]; memcpy(m->uvArray, uv.data(), uv.size() * sizeof(float));
	strcpy(m->name, "Capsule");
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

// ---- built-in foliage placeholders (7.4) --------------------------------------------------
// Small triangle-list builder shared by the foliage meshes (unindexed, like every builtin).
namespace {
struct TriBuilder
{
	std::vector<float> v, n, uv;
	void Tri(const float a[3], const float b[3], const float c[3],
	         const float na[3], const float nb[3], const float nc[3],
	         const float ta[2], const float tb[2], const float tc[2])
	{
		const float* P[3] = { a, b, c }; const float* N[3] = { na, nb, nc }; const float* T[3] = { ta, tb, tc };
		for (int i = 0; i < 3; ++i)
		{
			v.insert(v.end(), { P[i][0], P[i][1], P[i][2] });
			n.insert(n.end(), { N[i][0], N[i][1], N[i][2] });
			uv.insert(uv.end(), { T[i][0], T[i][1] });
		}
	}
	Mesh* Bake(const char* nm)
	{
		Mesh* m = new Mesh();
		m->numVerts = (int)(v.size() / 3);
		m->vertexArray = new float[v.size()];  memcpy(m->vertexArray, v.data(), v.size() * sizeof(float));
		m->normalArray = new float[n.size()];  memcpy(m->normalArray, n.data(), n.size() * sizeof(float));
		m->uvArray     = new float[uv.size()]; memcpy(m->uvArray, uv.data(), uv.size() * sizeof(float));
		strcpy(m->name, nm);
		return m;
	}
};
inline float FolHash(int i, int k) { float s = sinf((float)(i * 127 + k * 311) * 12.9898f) * 43758.5453f; return s - floorf(s); }
}  // namespace

// A clump of tapered grass blades. All normals point UP — the classic grass-lighting trick
// (blades shade like the ground under them instead of flickering per-face). Double-sided
// by mirrored winding, so it renders with the default backface-culled world pipeline.
Mesh* Mesh::CreateGrassClump()
{
	TriBuilder tb;
	const float up[3] = { 0, 1, 0 };
	const int kBlades = 7;
	for (int i = 0; i < kBlades; ++i)
	{
		const float ang  = (float)i / kBlades * 6.2831853f + FolHash(i, 0) * 0.8f;
		const float dist = 0.02f + FolHash(i, 1) * 0.10f;                 // clump radius
		const float h    = 0.28f + FolHash(i, 2) * 0.18f;                 // blade height
		const float lean = 0.04f + FolHash(i, 3) * 0.10f;                 // top lean (world units)
		const float w0   = 0.020f + FolHash(i, 4) * 0.012f;              // base half-width
		const float bx = cosf(ang) * dist, bz = sinf(ang) * dist;
		const float lx = cosf(ang + 1.3f) * lean, lz = sinf(ang + 1.3f) * lean;
		// width axis perpendicular to the lean direction
		const float wx = -sinf(ang + 1.3f), wz = cosf(ang + 1.3f);
		// three levels: base(w0) -> mid(0.55*w0, half lean) -> tip(point, full lean)
		const float L0[2][3] = { { bx - wx * w0, 0, bz - wz * w0 }, { bx + wx * w0, 0, bz + wz * w0 } };
		const float w1 = w0 * 0.55f;
		const float L1[2][3] = { { bx + lx * 0.4f - wx * w1, h * 0.55f, bz + lz * 0.4f - wz * w1 },
		                         { bx + lx * 0.4f + wx * w1, h * 0.55f, bz + lz * 0.4f + wz * w1 } };
		const float TIP[3] = { bx + lx, h, bz + lz };
		const float uvA[2] = { 0, 1 }, uvB[2] = { 1, 1 }, uvC[2] = { 0, 0.45f }, uvD[2] = { 1, 0.45f }, uvT[2] = { 0.5f, 0 };
		// front
		tb.Tri(L0[0], L0[1], L1[1], up, up, up, uvA, uvB, uvD);
		tb.Tri(L0[0], L1[1], L1[0], up, up, up, uvA, uvD, uvC);
		tb.Tri(L1[0], L1[1], TIP,   up, up, up, uvC, uvD, uvT);
		// back (mirrored winding = double-sided under backface culling)
		tb.Tri(L0[1], L0[0], L1[0], up, up, up, uvB, uvA, uvC);
		tb.Tri(L0[1], L1[0], L1[1], up, up, up, uvB, uvC, uvD);
		tb.Tri(L1[1], L1[0], TIP,   up, up, up, uvD, uvC, uvT);
	}
	return tb.Bake("GrassClump");
}

// A lumpy hemisphere-ish blob: lat-long sphere with hashed radius, flattened base.
Mesh* Mesh::CreateBush()
{
	TriBuilder tb;
	const int SEG = 8, RING = 5;
	const float R = 0.45f;
	auto pt = [&](int s, int r, float* out, float* nrm)
	{
		const float phi = (float)r / RING * 3.14159265f;          // 0 = top, pi = bottom
		const float th  = (float)(s % SEG) / SEG * 6.2831853f;
		const float rad = R * (0.82f + FolHash(s % SEG, r) * 0.30f);
		float x = sinf(phi) * cosf(th), y = cosf(phi), z = sinf(phi) * sinf(th);
		out[0] = x * rad; out[1] = y * rad * 0.75f + R * 0.72f; out[2] = z * rad;
		if (out[1] < 0.02f) out[1] = 0.02f;                       // flattened base
		nrm[0] = x; nrm[1] = y; nrm[2] = z;
	};
	for (int r = 0; r < RING; ++r)
		for (int s = 0; s < SEG; ++s)
		{
			float a[3], b[3], c[3], d[3], na[3], nb[3], nc[3], nd[3];
			pt(s, r, a, na); pt(s + 1, r, b, nb); pt(s + 1, r + 1, c, nc); pt(s, r + 1, d, nd);
			const float u0 = (float)s / SEG, u1 = (float)(s + 1) / SEG;
			const float v0 = (float)r / RING, v1 = (float)(r + 1) / RING;
			const float ta[2] = { u0, v0 }, tbv[2] = { u1, v0 }, tc[2] = { u1, v1 }, td[2] = { u0, v1 };
			tb.Tri(a, c, b, na, nc, nb, ta, tc, tbv);
			tb.Tri(a, d, c, na, nd, nc, ta, td, tc);
		}
	return tb.Bake("Bush");
}

// Trunk cylinder + a lumpy canopy blob. One material (a builtin placeholder, not an oak).
Mesh* Mesh::CreateTree()
{
	TriBuilder tb;
	const int SEG = 6;
	const float TR = 0.09f, TH = 1.1f;
	for (int s = 0; s < SEG; ++s)   // trunk
	{
		const float t0 = (float)s / SEG * 6.2831853f, t1 = (float)(s + 1) / SEG * 6.2831853f;
		const float x0 = cosf(t0) * TR, z0 = sinf(t0) * TR, x1 = cosf(t1) * TR, z1 = sinf(t1) * TR;
		const float a[3] = { x0, 0, z0 }, b[3] = { x1, 0, z1 }, c[3] = { x1 * 0.8f, TH, z1 * 0.8f }, d[3] = { x0 * 0.8f, TH, z0 * 0.8f };
		const float na[3] = { cosf(t0), 0, sinf(t0) }, nb[3] = { cosf(t1), 0, sinf(t1) };
		const float ta[2] = { (float)s / SEG, 1 }, tbv[2] = { (float)(s + 1) / SEG, 1 };
		const float tc[2] = { (float)(s + 1) / SEG, 0.45f }, td[2] = { (float)s / SEG, 0.45f };
		tb.Tri(a, b, c, na, nb, nb, ta, tbv, tc);
		tb.Tri(a, c, d, na, nb, na, ta, tc, td);
	}
	{	// canopy: lumpy sphere centered above the trunk
		const int CS = 8, CR = 5;
		const float CRAD = 0.55f, CY = TH + 0.35f;
		auto pt = [&](int s, int r, float* out, float* nrm)
		{
			const float phi = (float)r / CR * 3.14159265f;
			const float th  = (float)(s % CS) / CS * 6.2831853f;
			const float rad = CRAD * (0.8f + FolHash(s % CS + 17, r) * 0.35f);
			float x = sinf(phi) * cosf(th), y = cosf(phi), z = sinf(phi) * sinf(th);
			out[0] = x * rad; out[1] = CY + y * rad * 0.85f; out[2] = z * rad;
			nrm[0] = x; nrm[1] = y; nrm[2] = z;
		};
		for (int r = 0; r < CR; ++r)
			for (int s = 0; s < CS; ++s)
			{
				float a[3], b[3], c[3], d[3], na[3], nb[3], nc[3], nd[3];
				pt(s, r, a, na); pt(s + 1, r, b, nb); pt(s + 1, r + 1, c, nc); pt(s, r + 1, d, nd);
				const float u0 = (float)s / CS, u1 = (float)(s + 1) / CS;
				const float v0 = (float)r / CR * 0.45f, v1 = (float)(r + 1) / CR * 0.45f;
				const float ta[2] = { u0, v0 }, tbv[2] = { u1, v0 }, tc[2] = { u1, v1 }, td[2] = { u0, v1 };
				tb.Tri(a, c, b, na, nc, nb, ta, tc, tbv);
				tb.Tri(a, d, c, na, nd, nc, ta, td, tc);
			}
	}
	return tb.Bake("Tree");
}
}  // namespace nuke