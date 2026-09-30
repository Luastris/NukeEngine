#include "API/Model/Mesh.h"
#include <meshoptimizer.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <array>
#include <functional>
#include <vector>
#include <map>
#include <set>
#include <cmath>
#include <cstdint>
#include <cstring>
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

float Mesh::RaycastUV(const Vector3& ro, const Vector3& rd, float& u, float& v) const
{
	u = v = 0.0f;
	if (!vertexArray || numVerts < 3) return -1.0f;
	double bestT = 1e30, bu = 0, bv = 0; int bTri = -1;
	const int tris = TriCount();
	for (int t = 0; t < tris; ++t)
	{
		const uint32_t i0 = TriIndex(t, 0), i1 = TriIndex(t, 1), i2 = TriIndex(t, 2);
		const float* p0 = vertexArray + (size_t)i0 * 3;
		const float* p1 = vertexArray + (size_t)i1 * 3;
		const float* p2 = vertexArray + (size_t)i2 * 3;
		const double e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
		const double e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
		const double px = rd.y * e2[2] - rd.z * e2[1], py = rd.z * e2[0] - rd.x * e2[2], pz = rd.x * e2[1] - rd.y * e2[0];
		const double det = e1[0] * px + e1[1] * py + e1[2] * pz;
		if (std::fabs(det) < 1e-12) continue;
		const double inv = 1.0 / det;
		const double tv[3] = { ro.x - p0[0], ro.y - p0[1], ro.z - p0[2] };
		const double uu = (tv[0] * px + tv[1] * py + tv[2] * pz) * inv;
		if (uu < 0.0 || uu > 1.0) continue;
		const double qx = tv[1] * e1[2] - tv[2] * e1[1], qy = tv[2] * e1[0] - tv[0] * e1[2], qz = tv[0] * e1[1] - tv[1] * e1[0];
		const double vv = (rd.x * qx + rd.y * qy + rd.z * qz) * inv;
		if (vv < 0.0 || uu + vv > 1.0) continue;
		const double tt = (e2[0] * qx + e2[1] * qy + e2[2] * qz) * inv;
		if (tt > 1e-6 && tt < bestT) { bestT = tt; bu = uu; bv = vv; bTri = t; }
	}
	if (bTri < 0) return -1.0f;
	if (uvArray)
	{
		const uint32_t i0 = TriIndex(bTri, 0), i1 = TriIndex(bTri, 1), i2 = TriIndex(bTri, 2);
		const float* t0 = uvArray + (size_t)i0 * 2;
		const float* t1 = uvArray + (size_t)i1 * 2;
		const float* t2 = uvArray + (size_t)i2 * 2;
		const float w0 = (float)(1.0 - bu - bv);
		u = t0[0] * w0 + t1[0] * (float)bu + t2[0] * (float)bv;
		v = t0[1] * w0 + t1[1] * (float)bu + t2[1] * (float)bv;
	}
	return (float)bestT;
}

// The shared indexed builder (see Mesh.h for the contract): merges the source parts into one
// sectioned, skinned, morph-carrying, cache-optimized, LOD-chained mesh.
Mesh* Mesh::Build(const MeshSource& src, std::vector<int>* outSlotMats)
{
	Mesh* m = new Mesh();
	strncpy(m->name, src.name.c_str(), sizeof(m->name) - 1);
	m->name[sizeof(m->name) - 1] = 0;

	// Material SLOTS: dedup source material indices in first-seen order.
	std::vector<int> slotMat;
	auto slotOf = [&slotMat](int mi) -> int
	{
		for (size_t s = 0; s < slotMat.size(); ++s) if (slotMat[s] == mi) return (int)s;
		slotMat.push_back(mi);
		return (int)slotMat.size() - 1;
	};

	bool anyBones = false;
	for (const MeshSourcePart& pt : src.parts) anyBones = anyBones || !pt.boneIdx.empty();
	const bool skin = anyBones && !src.skeleton.empty();
	if (skin && src.embedSkeleton) m->bones = src.skeleton;

	size_t totalV = 0;
	bool anyUV = false, anyTan = false, anyUV2 = false, anyCol = false;
	for (const MeshSourcePart& pt : src.parts)
	{
		totalV += pt.pos.size() / 3;
		anyUV  = anyUV  || !pt.uv.empty();
		anyTan = anyTan || !pt.tan.empty();
		anyUV2 = anyUV2 || !pt.uv2.empty();
		anyCol = anyCol || !pt.col.empty();
	}
	if (totalV == 0) return m;

	std::vector<float> pos(totalV * 3, 0.f), nrm(totalV * 3, 0.f);
	std::vector<float> uv (anyUV  ? totalV * 2 : 0, 0.f);
	std::vector<float> tan(anyTan ? totalV * 4 : 0, 0.f);
	std::vector<float> uv2(anyUV2 ? totalV * 2 : 0, 0.f);
	std::vector<float> col(anyCol ? totalV * 4 : 0, 1.f);
	std::vector<unsigned short> bIdx(skin ? totalV * 4 : 0, 0);
	std::vector<float>          bWgt(skin ? totalV * 4 : 0, 0.f);
	std::vector<uint32_t> idx;

	// Morph targets, merged by NAME across the parts: dense deltas over the whole vertex range.
	std::vector<std::string> morphNames;
	std::vector<std::vector<float>> morphPos, morphNrm;
	auto morphOf = [&](const std::string& nm) -> int
	{
		const std::string n2 = nm.empty() ? ("morph" + std::to_string(morphNames.size())) : nm;
		for (size_t i = 0; i < morphNames.size(); ++i) if (morphNames[i] == n2) return (int)i;
		morphNames.push_back(n2);
		morphPos.emplace_back(totalV * 3, 0.f);
		morphNrm.emplace_back(totalV * 3, 0.f);
		return (int)morphNames.size() - 1;
	};

	uint32_t vbase = 0;
	std::vector<int> partSections;   // section index per part (-1 = no triangles)
	for (const MeshSourcePart& pt : src.parts)
	{
		const uint32_t nv = (uint32_t)(pt.pos.size() / 3);
		auto copyStream = [&](const std::vector<float>& in, std::vector<float>& out, size_t comps)
		{
			if (in.empty() || out.empty()) return;
			const size_t n = std::min((size_t)nv * comps, in.size());
			memcpy(&out[(size_t)vbase * comps], in.data(), n * sizeof(float));
		};
		copyStream(pt.pos, pos, 3); copyStream(pt.nrm, nrm, 3);
		copyStream(pt.uv, uv, 2);   copyStream(pt.tan, tan, 4);
		copyStream(pt.uv2, uv2, 2); copyStream(pt.col, col, 4);
		if (skin && !pt.boneIdx.empty())
		{
			const size_t n = std::min((size_t)nv * 4, pt.boneIdx.size());
			for (size_t k = 0; k < n; ++k) { bIdx[(size_t)vbase * 4 + k] = pt.boneIdx[k]; bWgt[(size_t)vbase * 4 + k] = k < pt.boneWgt.size() ? pt.boneWgt[k] : 0.f; }
		}
		for (const MeshSourcePart::Morph& mo : pt.morphs)   // blend shapes
		{
			const int mi2 = morphOf(mo.name);
			const size_t n = std::min((size_t)nv * 3, mo.posDelta.size());
			for (size_t k = 0; k < n; ++k) morphPos[mi2][(size_t)vbase * 3 + k] = mo.posDelta[k];
			const size_t nn = std::min((size_t)nv * 3, mo.nrmDelta.size());
			for (size_t k = 0; k < nn; ++k) morphNrm[mi2][(size_t)vbase * 3 + k] = mo.nrmDelta[k];
		}
		MeshSection sec;
		sec.firstIndex = (uint32_t)idx.size();
		sec.slot = slotOf(pt.material);
		for (size_t f = 0; f + 2 < pt.indices.size(); f += 3)
		{
			if (pt.indices[f] >= nv || pt.indices[f + 1] >= nv || pt.indices[f + 2] >= nv) continue;
			idx.push_back(vbase + pt.indices[f]); idx.push_back(vbase + pt.indices[f + 1]); idx.push_back(vbase + pt.indices[f + 2]);
		}
		sec.indexCount = (uint32_t)idx.size() - sec.firstIndex;
		if (sec.indexCount > 0) { partSections.push_back((int)m->sections.size()); m->sections.push_back(sec); }
		else partSections.push_back(-1);
		vbase += nv;
	}
	if (idx.empty()) { m->sections.clear(); return m; }

	if (skin)   // normalize the weight quads
		for (size_t d = 0; d < totalV; ++d)
		{
			float sum = bWgt[d * 4] + bWgt[d * 4 + 1] + bWgt[d * 4 + 2] + bWgt[d * 4 + 3];
			if (sum <= 0.0f) continue;   // unweighted vertex (a rigid part in the merge): stays at bind
			for (int k = 0; k < 4; ++k) bWgt[d * 4 + k] /= sum;
		}

	// Per-part inverse binds: when THIS mesh's binds disagree with the shared skeleton's
	// canonical set (glTF bakes every skin in its own space), embed the skeleton with the
	// part's OWN binds swapped in; the skinning palette prefers embedded binds.
	if (skin && !src.embedSkeleton)
	{
		bool differs = false;
		std::map<int, const std::array<float, 16>*> own;
		for (const MeshSourcePart& pt : src.parts)
			for (const auto& kv : pt.ownInvBind)
			{
				if (kv.first < 0 || kv.first >= (int)src.skeleton.size()) continue;
				own[kv.first] = &kv.second;
				const float* skv = src.skeleton[kv.first].invBind;
				for (int k = 0; k < 16 && !differs; ++k)
					if (std::fabs(kv.second[k] - skv[k]) > 1e-3f) differs = true;
			}
		if (differs)
		{
			m->bones = src.skeleton;   // canonical hierarchy/locals, own inverse binds
			for (const auto& kv : own) for (int k = 0; k < 16; ++k) m->bones[kv.first].invBind[k] = (*kv.second)[k];
		}
	}

	// --- meshoptimizer: vertex-cache order per section, then the LOD chain ------------------
	for (const MeshSection& s : m->sections)
		meshopt_optimizeVertexCache(idx.data() + s.firstIndex, idx.data() + s.firstIndex, s.indexCount, totalV);

	// Authored LODs (source _LOD0/_LOD1/... parts): the levels ship as-is, one MeshLOD per
	// level over its contiguous section run - the artist's chain wins over auto-LOD.
	bool authoredLods = false;
	if (src.authoredLods)
	{
		int maxLevel = 0;
		for (const MeshSourcePart& pt : src.parts) maxLevel = std::max(maxLevel, pt.lodLevel);
		if (maxLevel > 0)
		{
			authoredLods = true;
			static const float kLevelScreen[] = { 0.0f, 0.35f, 0.15f, 0.06f, 0.02f };
			int sec = 0;
			for (int level = 0; level <= maxLevel; ++level)
			{
				MeshLOD ml;
				ml.firstSection = sec;
				for (size_t i = 0; i < src.parts.size(); ++i)
					if (src.parts[i].lodLevel == level && partSections[i] >= 0) ++sec;
				ml.sectionCount = sec - ml.firstSection;
				ml.screenSize = level < 5 ? kLevelScreen[level] : 0.01f;
				if (ml.sectionCount > 0) m->lods.push_back(ml);
			}
		}
	}

	const int lod0Count = (int)m->sections.size();
	if (!authoredLods)
	{
		MeshLOD l0; l0.firstSection = 0; l0.sectionCount = lod0Count; l0.screenSize = 0.0f;
		m->lods.push_back(l0);
	}
	// Per-level target tri ratios (of LOD0) and the screen coverage below which the level kicks in.
	const float kRatio[]  = { 0.45f, 0.20f, 0.08f };
	const float kScreen[] = { 0.35f, 0.15f, 0.06f };
	size_t prevTris = idx.size() / 3;
	for (int l = 0; l < 3 && !authoredLods; ++l)
	{
		std::vector<MeshSection> secs;
		std::vector<uint32_t> lidx;
		for (int s0 = 0; s0 < lod0Count; ++s0)
		{
			const MeshSection& s = m->sections[s0];
			size_t target = (size_t)(s.indexCount * kRatio[l]);
			target -= target % 3;
			if (target < 3 * 4) target = 3 * 4;   // never simplify below 4 tris per section
			if (target >= s.indexCount) target = s.indexCount;
			std::vector<uint32_t> dst(s.indexCount);
			float err = 0.0f;
			size_t got = meshopt_simplify(dst.data(), idx.data() + s.firstIndex, s.indexCount,
			                              pos.data(), totalV, sizeof(float) * 3,
			                              target, 0.05f, 0, &err);
			dst.resize(got);
			if (got >= 3)
			{
				meshopt_optimizeVertexCache(dst.data(), dst.data(), got, totalV);
				MeshSection ns; ns.firstIndex = (uint32_t)lidx.size(); ns.indexCount = (uint32_t)got; ns.slot = s.slot;
				secs.push_back(ns);
				lidx.insert(lidx.end(), dst.begin(), dst.end());
			}
		}
		const size_t levelTris = lidx.size() / 3;
		// The chain stops paying: barely smaller than the previous level, or already tiny.
		if (secs.empty() || levelTris >= prevTris * 85 / 100 || levelTris < 16) break;
		const uint32_t base = (uint32_t)idx.size();
		for (MeshSection& ns : secs) ns.firstIndex += base;
		MeshLOD ml; ml.firstSection = (int)m->sections.size(); ml.sectionCount = (int)secs.size(); ml.screenSize = kScreen[l];
		m->lods.push_back(ml);
		m->sections.insert(m->sections.end(), secs.begin(), secs.end());
		idx.insert(idx.end(), lidx.begin(), lidx.end());
		prevTris = levelTris;
	}

	// Vertex-FETCH optimization over the final index buffer; remap every stream (drops unused verts).
	std::vector<unsigned int> remap(totalV);
	const size_t unique = meshopt_optimizeVertexFetchRemap(remap.data(), idx.data(), idx.size(), totalV);
	meshopt_remapIndexBuffer(idx.data(), idx.data(), idx.size(), remap.data());
	auto remapStream = [&](std::vector<float>& v, size_t comps)
	{
		if (v.empty()) return;
		std::vector<float> out(unique * comps);
		meshopt_remapVertexBuffer(out.data(), v.data(), totalV, sizeof(float) * comps, remap.data());
		v.swap(out);
	};
	remapStream(pos, 3); remapStream(nrm, 3); remapStream(uv, 2);
	remapStream(tan, 4); remapStream(uv2, 2); remapStream(col, 4);
	remapStream(bWgt, 4);
	for (size_t mi2 = 0; mi2 < morphNames.size(); ++mi2)
	{ remapStream(morphPos[mi2], 3); remapStream(morphNrm[mi2], 3); }
	if (!bIdx.empty())
	{
		std::vector<unsigned short> out(unique * 4);
		meshopt_remapVertexBuffer(out.data(), bIdx.data(), totalV, sizeof(unsigned short) * 4, remap.data());
		bIdx.swap(out);
	}

	// --- commit ----------------------------------------------------------------------------
	m->numVerts   = (int)unique;
	m->numIndices = (int)idx.size();
	m->indexArray = new uint32_t[idx.size()];
	memcpy(m->indexArray, idx.data(), idx.size() * sizeof(uint32_t));
	auto commit = [&](const std::vector<float>& v) -> float*
	{
		if (v.empty()) return nullptr;
		float* p = new float[v.size()];
		memcpy(p, v.data(), v.size() * sizeof(float));
		return p;
	};
	m->vertexArray  = commit(pos);
	m->normalArray  = commit(nrm);
	m->uvArray      = anyUV ? commit(uv) : nullptr;
	m->tangentArray = anyTan ? commit(tan) : nullptr;
	m->uv2Array     = anyUV2 ? commit(uv2) : nullptr;
	m->colorArray   = anyCol ? commit(col) : nullptr;
	if (skin)
	{
		m->boneIndex = new unsigned short[unique * 4];
		memcpy(m->boneIndex, bIdx.data(), unique * 4 * sizeof(unsigned short));
		m->boneWeight = new float[unique * 4];
		memcpy(m->boneWeight, bWgt.data(), unique * 4 * sizeof(float));
	}
	for (size_t mi2 = 0; mi2 < morphNames.size(); ++mi2)
	{
		Mesh::MorphTarget mt;
		mt.name = morphNames[mi2];
		mt.posDelta = std::move(morphPos[mi2]);
		bool anyN = false;
		for (float f2 : morphNrm[mi2]) if (f2 != 0.0f) { anyN = true; break; }
		if (anyN) mt.nrmDelta = std::move(morphNrm[mi2]);
		m->morphs.push_back(std::move(mt));
	}
	m->numSlots = slotMat.empty() ? 1 : (int)slotMat.size();
	for (int mi : slotMat)
		m->slotNames.push_back(mi >= 0 && mi < (int)src.materialNames.size() ? src.materialNames[mi] : "");
	if (outSlotMats) *outSlotMats = slotMat;
	return m;
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

// Append one triangle-souped quad (a,b,c,d) as two tris (a,b,c)(a,c,d), with the SAME winding
// CreateSphere uses (a=upper, b=lower, c=lower next, d=upper next).
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
	// A point on a hemisphere of radius R centred at (0,yc,0); normal = dir.
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
// Layout: magic "NUMESH\0\0" | u32 version | str name | str guid | i32 numVerts | f32 pos[3N] |
//         u8 hasNormals (+ f32 nrm[3N]) | u8 hasUV (+ f32 uv[2N]).
// v2 appends the SKIN block (bones + boneIndex[4N] + boneWeight[4N]); v3 widened boneIndex u8 -> u16.
// v4 appends the INDEXED block: u32 numIndices (+ u32 idx[]), optional tangent/uv2/color streams,
//    sections + LODs + material slots.
namespace {
	const char  kMagic[8] = { 'N','U','M','E','S','H','\0','\0' };
	const uint32_t kVersion = 8;   // v5 skelGuid; v6 morph targets; v7 import-time materials; v8 baked fracture pieces
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
	// --- v4: indexed block -----------------------------------------------------------------
	uint32_t ni = (uint32_t)(indexArray ? numIndices : 0); wr(o, ni);
	if (ni) o.write((const char*)indexArray, sizeof(uint32_t) * ni);
	uint8_t hasTan = (tangentArray != nullptr) ? 1 : 0; wr(o, hasTan);
	if (hasTan) o.write((const char*)tangentArray, sizeof(float) * 4 * n);
	uint8_t hasUV2 = (uv2Array != nullptr) ? 1 : 0; wr(o, hasUV2);
	if (hasUV2) o.write((const char*)uv2Array, sizeof(float) * 2 * n);
	uint8_t hasCol = (colorArray != nullptr) ? 1 : 0; wr(o, hasCol);
	if (hasCol) o.write((const char*)colorArray, sizeof(float) * 4 * n);
	uint32_t sc = (uint32_t)sections.size(); wr(o, sc);
	for (const MeshSection& s : sections) { wr(o, s.firstIndex); wr(o, s.indexCount); int32_t sl = s.slot; wr(o, sl); }
	uint32_t lc = (uint32_t)lods.size(); wr(o, lc);
	for (const MeshLOD& l : lods) { int32_t fs = l.firstSection, scn = l.sectionCount; wr(o, fs); wr(o, scn); wr(o, l.screenSize); }
	int32_t slots = numSlots; wr(o, slots);
	uint32_t nn2 = (uint32_t)slotNames.size(); wr(o, nn2);
	for (const std::string& s : slotNames) wrStr(o, s);
	// --- v5: skeleton-asset reference ------------------------------------------------------
	wrStr(o, skelGuid);
	// --- v6: morph targets -------------------------------------------------------------------
	uint32_t mc = (uint32_t)morphs.size(); wr(o, mc);
	for (const MorphTarget& mt : morphs)
	{
		wrStr(o, mt.name);
		uint8_t hasP = mt.posDelta.size() == (size_t)n * 3 ? 1 : 0; wr(o, hasP);
		if (hasP) o.write((const char*)mt.posDelta.data(), sizeof(float) * 3 * n);
		uint8_t hasN = mt.nrmDelta.size() == (size_t)n * 3 ? 1 : 0; wr(o, hasN);
		if (hasN) o.write((const char*)mt.nrmDelta.data(), sizeof(float) * 3 * n);
	}
	// --- v7: import-time material per SLOT ----------------------------------------------------
	uint32_t dm = (uint32_t)defaultMats.size(); wr(o, dm);
	for (const std::string& g : defaultMats) wrStr(o, g);
	// --- v8: baked fracture pieces (Destructible) ------------------------------------------------
	int32_t fpc = fracturePieces; wr(o, fpc);
	uint32_t fseed = fractureSeed; wr(o, fseed);
	uint32_t fn = (uint32_t)fracture.size(); wr(o, fn);
	for (const FracturePiece& fp : fracture)
	{
		uint32_t nv = (uint32_t)(fp.verts.size() / 3); wr(o, nv);
		if (nv) { o.write((const char*)fp.verts.data(), sizeof(float) * 3 * nv); o.write((const char*)fp.normals.data(), sizeof(float) * 3 * nv); o.write((const char*)fp.uvs.data(), sizeof(float) * 2 * nv); }
		uint32_t sv = (uint32_t)fp.surfVerts; wr(o, sv);
		float c[3] = { (float)fp.centroid.x, (float)fp.centroid.y, (float)fp.centroid.z }; o.write((const char*)c, sizeof(c));
		float h[3] = { (float)fp.halfExtents.x, (float)fp.halfExtents.y, (float)fp.halfExtents.z }; o.write((const char*)h, sizeof(h));
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

int Mesh::FormatVersion() { return (int)kVersion; }

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
	if (version >= 4)   // indexed block (older files simply have none)
	{
		uint32_t ni = 0; rd(i, ni);
		if (ni)
		{
			m->numIndices = (int)ni;
			m->indexArray = new uint32_t[ni];
			i.read((char*)m->indexArray, sizeof(uint32_t) * ni);
		}
		uint8_t hasTan = 0; rd(i, hasTan);
		if (hasTan && n > 0) { m->tangentArray = new float[(size_t)n * 4]; i.read((char*)m->tangentArray, sizeof(float) * 4 * n); }
		uint8_t hasUV2 = 0; rd(i, hasUV2);
		if (hasUV2 && n > 0) { m->uv2Array = new float[(size_t)n * 2]; i.read((char*)m->uv2Array, sizeof(float) * 2 * n); }
		uint8_t hasCol = 0; rd(i, hasCol);
		if (hasCol && n > 0) { m->colorArray = new float[(size_t)n * 4]; i.read((char*)m->colorArray, sizeof(float) * 4 * n); }
		uint32_t sc = 0; rd(i, sc);
		m->sections.resize(sc);
		for (uint32_t k = 0; k < sc; ++k)
		{
			rd(i, m->sections[k].firstIndex); rd(i, m->sections[k].indexCount);
			int32_t sl = 0; rd(i, sl); m->sections[k].slot = sl;
		}
		uint32_t lc = 0; rd(i, lc);
		m->lods.resize(lc);
		for (uint32_t k = 0; k < lc; ++k)
		{
			int32_t fs = 0, scn = 0; rd(i, fs); rd(i, scn);
			m->lods[k].firstSection = fs; m->lods[k].sectionCount = scn;
			rd(i, m->lods[k].screenSize);
		}
		int32_t slots = 1; rd(i, slots); m->numSlots = slots < 1 ? 1 : slots;
		uint32_t nn2 = 0; rd(i, nn2);
		m->slotNames.resize(nn2);
		for (uint32_t k = 0; k < nn2; ++k) m->slotNames[k] = rdStr(i);

		// Sanitize: a corrupt/hand-edited v4 block must never reach the GPU/RT paths with
		// out-of-range indices or ranges. Bad data degrades to the plain vertex soup.
		bool bad = false;
		for (int k = 0; k < m->numIndices && !bad; ++k)
			if (m->indexArray[k] >= (uint32_t)m->numVerts) bad = true;
		for (const MeshSection& s : m->sections)
			if ((uint64_t)s.firstIndex + s.indexCount > (uint64_t)(m->numIndices > 0 ? m->numIndices : m->numVerts)
			    || s.indexCount % 3 != 0) { bad = true; break; }
		for (const MeshLOD& l : m->lods)
			if (l.firstSection < 0 || l.sectionCount < 0
			    || l.firstSection + l.sectionCount > (int)m->sections.size()) { bad = true; break; }
		if (bad)
		{
			std::cout << "[Mesh]\t\t'" << m->name << "' has a corrupt v4 block — indexed data dropped" << std::endl;
			delete[] m->indexArray; m->indexArray = nullptr; m->numIndices = 0;
			m->sections.clear(); m->lods.clear(); m->numSlots = 1; m->slotNames.clear();
		}
	}
	if (version >= 5) m->skelGuid = rdStr(i);
	if (version >= 6)
	{
		uint32_t mc = 0; rd(i, mc);
		if (mc <= 4096)   // sanity — a corrupt count must not allocate the moon
			for (uint32_t k = 0; k < mc && i; ++k)
			{
				Mesh::MorphTarget mt;
				mt.name = rdStr(i);
				uint8_t hasP = 0; rd(i, hasP);
				if (hasP && n > 0) { mt.posDelta.resize((size_t)n * 3); i.read((char*)mt.posDelta.data(), sizeof(float) * 3 * n); }
				uint8_t hasN = 0; rd(i, hasN);
				if (hasN && n > 0) { mt.nrmDelta.resize((size_t)n * 3); i.read((char*)mt.nrmDelta.data(), sizeof(float) * 3 * n); }
				m->morphs.push_back(std::move(mt));
			}
	}
	if (version >= 7)
	{
		uint32_t dm = 0; rd(i, dm);
		if (dm <= 4096)
			for (uint32_t k = 0; k < dm && i; ++k) m->defaultMats.push_back(rdStr(i));
	}
	if (version >= 8)
	{
		int32_t fpc = 0; rd(i, fpc); m->fracturePieces = fpc;
		uint32_t fseed = 0; rd(i, fseed); m->fractureSeed = fseed;
		uint32_t fn = 0; rd(i, fn);
		if (fn <= 4096)
			for (uint32_t k = 0; k < fn && i; ++k)
			{
				FracturePiece fp;
				uint32_t nv = 0; rd(i, nv);
				if (nv > 0 && nv <= 4u * 1024u * 1024u)
				{
					fp.verts.resize((size_t)nv * 3); fp.normals.resize((size_t)nv * 3); fp.uvs.resize((size_t)nv * 2);
					i.read((char*)fp.verts.data(), sizeof(float) * 3 * nv); i.read((char*)fp.normals.data(), sizeof(float) * 3 * nv); i.read((char*)fp.uvs.data(), sizeof(float) * 2 * nv);
				}
				uint32_t sv = 0; rd(i, sv); fp.surfVerts = sv;
				float c[3], h[3]; i.read((char*)c, sizeof(c)); i.read((char*)h, sizeof(h));
				fp.centroid = Vector3(c[0], c[1], c[2]); fp.halfExtents = Vector3(h[0], h[1], h[2]);
				m->fracture.push_back(std::move(fp));
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

// A clump of tapered grass blades. All normals point UP (blades shade like the ground under
// them); double-sided by mirrored winding, so backface culling stays on.
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