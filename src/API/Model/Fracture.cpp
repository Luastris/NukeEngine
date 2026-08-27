// Runtime Voronoi mesh fracture (see Fracture.h): plane-clip the real triangles into the
// cells, cap the cuts with the cell's bisector faces, reject caps outside the mesh volume.
#include "API/Model/Fracture.h"
#include "API/Model/Mesh.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nuke {

namespace {

struct Plane { Vector3 n; double d; };   // keep side: n·x <= d

struct ClipVert { Vector3 p; Vector3 nrm; float u, v; };

double Dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vector3 Cross(Vector3 a, Vector3 b)
{ return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
Vector3 Norm(Vector3 v)
{
	const double l = std::sqrt(Dot(v, v));
	return l > 1e-12 ? Vector3(v.x / l, v.y / l, v.z / l) : Vector3(0, 1, 0);
}

ClipVert Lerp(ClipVert a, ClipVert b, double t)   // by value: Vector3 operators are non-const
{
	ClipVert r;
	r.p = a.p + (b.p - a.p) * t;
	r.nrm = a.nrm + (b.nrm - a.nrm) * t;
	r.u = (float)(a.u + (b.u - a.u) * t);
	r.v = (float)(a.v + (b.v - a.v) * t);
	return r;
}

// Sutherland-Hodgman: keep the n·x <= d side.
void ClipPoly(std::vector<ClipVert>& poly, const Plane& pl, std::vector<ClipVert>& tmp)
{
	tmp.clear();
	const size_t n = poly.size();
	for (size_t i = 0; i < n; ++i)
	{
		const ClipVert& a = poly[i];
		const ClipVert& b = poly[(i + 1) % n];
		const double da = Dot(pl.n, a.p) - pl.d;
		const double db = Dot(pl.n, b.p) - pl.d;
		const bool ina = da <= 1e-9, inb = db <= 1e-9;
		if (ina) tmp.push_back(a);
		if (ina != inb)
		{
			const double t = da / (da - db);
			tmp.push_back(Lerp(a, b, t));
		}
	}
	poly.swap(tmp);
}

// Parity test against the source triangles (+X ray): rejects cap faces that fall outside a
// CONCAVE mesh — convex sources pass exactly.
bool InsideMesh(const Vector3& p, const std::vector<float>& tris)
{
	int hits = 0;
	const size_t n = tris.size() / 9;
	for (size_t t = 0; t < n; ++t)
	{
		const float* v0 = &tris[t * 9];
		const float* v1 = v0 + 3;
		const float* v2 = v0 + 6;
		// Moller-Trumbore along +X.
		const double e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
		const double e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];
		// dir = (1,0,0): h = dir x e2 = (0, -e2z, e2y)
		const double a = e1y * (-e2z) + e1z * e2y;
		if (std::fabs(a) < 1e-12) continue;
		const double f = 1.0 / a;
		const double sx = p.x - v0[0], sy = p.y - v0[1], sz = p.z - v0[2];
		const double u = f * (sy * (-e2z) + sz * e2y);
		if (u < 0.0 || u > 1.0) continue;
		// q = s x e1
		const double qx = sy * e1z - sz * e1y;
		const double qy = sz * e1x - sx * e1z;
		const double qz = sx * e1y - sy * e1x;
		const double v = f * qx;
		if (v < 0.0 || u + v > 1.0) continue;
		const double dist = f * (e2x * qx + e2y * qy + e2z * qz);
		if (dist > 1e-9) ++hits;
	}
	return (hits & 1) != 0;
}

}  // namespace

bool FractureMesh(const Mesh* src, const Vector3& scale, int pieces, uint32_t seed,
                  std::vector<FracturePiece>& out)
{
	out.clear();
	if (!src || !src->vertexArray) return false;
	const int triCount = src->TriCount();
	if (triCount < 1) return false;
	const int n = std::max(2, std::min(64, pieces));

	// Scaled triangle soup (positions), plus the source attribute fetch.
	std::vector<float> soup;
	soup.resize((size_t)triCount * 9);
	for (int t = 0; t < triCount; ++t)
		for (int k = 0; k < 3; ++k)
		{
			const uint32_t idx = src->TriIndex(t, k);
			soup[(size_t)t * 9 + k * 3 + 0] = src->vertexArray[(size_t)idx * 3 + 0] * (float)scale.x;
			soup[(size_t)t * 9 + k * 3 + 1] = src->vertexArray[(size_t)idx * 3 + 1] * (float)scale.y;
			soup[(size_t)t * 9 + k * 3 + 2] = src->vertexArray[(size_t)idx * 3 + 2] * (float)scale.z;
		}
	Vector3 mn(1e30, 1e30, 1e30), mx(-1e30, -1e30, -1e30);
	for (size_t i = 0; i < soup.size(); i += 3)
	{
		mn.x = std::min(mn.x, (double)soup[i]);     mx.x = std::max(mx.x, (double)soup[i]);
		mn.y = std::min(mn.y, (double)soup[i + 1]); mx.y = std::max(mx.y, (double)soup[i + 1]);
		mn.z = std::min(mn.z, (double)soup[i + 2]); mx.z = std::max(mx.z, (double)soup[i + 2]);
	}
	Vector3 size = mx - mn;
	if (size.x + size.y + size.z < 1e-6) return false;

	// Seeds: deterministic LCG points in the bounds.
	uint64_t rng = seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
	auto r01 = [&]() { rng = rng * 6364136223846793005ull + 1442695040888963407ull;
	                   return (double)((rng >> 33) & 0x7FFFFFFF) / 2147483647.0; };
	std::vector<Vector3> seeds(n);
	for (int i = 0; i < n; ++i)
		seeds[i] = Vector3(mn.x + r01() * size.x, mn.y + r01() * size.y, mn.z + r01() * size.z);

	const double diag = std::sqrt(Dot(size, size));
	std::vector<ClipVert> poly, tmp;

	// Unique surface planes (quantized dedupe): cap faces clip against these too, or the cell
	// faces poke past the mesh where no bisector bounds them (the stretched-sliver artifact).
	// Convex sources clip exactly; concave ones shrink toward the hull — honest v1.
	std::vector<Plane> surfPlanes;
	{
		std::vector<uint64_t> keys;
		for (int t = 0; t < triCount && (int)surfPlanes.size() < 128; ++t)
		{
			Vector3 a(soup[(size_t)t * 9], soup[(size_t)t * 9 + 1], soup[(size_t)t * 9 + 2]);
			Vector3 b(soup[(size_t)t * 9 + 3], soup[(size_t)t * 9 + 4], soup[(size_t)t * 9 + 5]);
			Vector3 c(soup[(size_t)t * 9 + 6], soup[(size_t)t * 9 + 7], soup[(size_t)t * 9 + 8]);
			Vector3 nn = Cross(b - a, c - a);
			if (Dot(nn, nn) < 1e-12) continue;
			nn = Norm(nn);
			const double d = Dot(nn, a);
			const uint64_t key = ((uint64_t)(int32_t)std::llround(nn.x * 512.0) & 0xFFFF)
			                   | (((uint64_t)(int32_t)std::llround(nn.y * 512.0) & 0xFFFF) << 16)
			                   | (((uint64_t)(int32_t)std::llround(nn.z * 512.0) & 0xFFFF) << 32)
			                   | (((uint64_t)(int32_t)std::llround(d / diag * 512.0) & 0xFFFF) << 48);
			if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
			keys.push_back(key);
			surfPlanes.push_back({ nn, d });
		}
	}

	for (int i = 0; i < n; ++i)
	{
		// The cell: bisector half-spaces against every other seed.
		std::vector<Plane> planes;
		planes.reserve(n - 1);
		for (int j = 0; j < n; ++j)
		{
			if (j == i) continue;
			Vector3 d = seeds[j] - seeds[i];
			if (Dot(d, d) < 1e-12) continue;
			Plane pl;
			pl.n = Norm(d);
			Vector3 m = (seeds[i] + seeds[j]) * 0.5;
			pl.d = Dot(pl.n, m);
			planes.push_back(pl);
		}

		FracturePiece piece;
		auto pushTri = [&](const ClipVert& a, const ClipVert& b, const ClipVert& c)
		{
			const ClipVert* v[3] = { &a, &b, &c };
			for (int k = 0; k < 3; ++k)
			{
				piece.verts.push_back((float)v[k]->p.x);
				piece.verts.push_back((float)v[k]->p.y);
				piece.verts.push_back((float)v[k]->p.z);
				Vector3 nn = Norm(v[k]->nrm);
				piece.normals.push_back((float)nn.x);
				piece.normals.push_back((float)nn.y);
				piece.normals.push_back((float)nn.z);
				piece.uvs.push_back(v[k]->u);
				piece.uvs.push_back(v[k]->v);
			}
		};

		// Surface: the real triangles clipped into the cell.
		for (int t = 0; t < triCount; ++t)
		{
			poly.clear();
			for (int k = 0; k < 3; ++k)
			{
				const uint32_t idx = src->TriIndex(t, k);
				ClipVert cv;
				cv.p = Vector3(soup[(size_t)t * 9 + k * 3], soup[(size_t)t * 9 + k * 3 + 1], soup[(size_t)t * 9 + k * 3 + 2]);
				cv.nrm = src->normalArray
				       ? Vector3(src->normalArray[(size_t)idx * 3], src->normalArray[(size_t)idx * 3 + 1], src->normalArray[(size_t)idx * 3 + 2])
				       : Vector3(0, 1, 0);
				cv.u = src->uvArray ? src->uvArray[(size_t)idx * 2] : 0.0f;
				cv.v = src->uvArray ? src->uvArray[(size_t)idx * 2 + 1] : 0.0f;
				poly.push_back(cv);
			}
			for (const Plane& pl : planes)
			{
				if (poly.size() < 3) break;
				ClipPoly(poly, pl, tmp);
			}
			for (size_t k = 2; k < poly.size(); ++k)
				pushTri(poly[0], poly[k - 1], poly[k]);
		}

		piece.surfVerts = piece.verts.size() / 3;   // caps append after the surface

		// Caps: each bisector face of the cell polytope, clipped by the other planes, kept when
		// its center sits inside the mesh. Winding faces the NEIGHBOUR (outward of this piece).
		for (size_t pi = 0; pi < planes.size(); ++pi)
		{
			const Plane& pl = planes[pi];
			Vector3 t1 = Norm(Cross(pl.n, std::fabs(pl.n.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0)));
			Vector3 t2 = Cross(pl.n, t1);
			Vector3 c = pl.n * pl.d;   // a point on the plane
			poly.clear();
			for (int k = 0; k < 4; ++k)
			{
				static const double sq[4][2] = { {-1, -1}, {1, -1}, {1, 1}, {-1, 1} };
				ClipVert cv;
				cv.p = c + t1 * (sq[k][0] * diag) + t2 * (sq[k][1] * diag);
				cv.nrm = pl.n;
				cv.u = (float)(sq[k][0] * diag);
				cv.v = (float)(sq[k][1] * diag);
				poly.push_back(cv);
			}
			for (size_t pj = 0; pj < planes.size(); ++pj)
			{
				if (pj == pi || poly.size() < 3) continue;
				ClipPoly(poly, planes[pj], tmp);
			}
			for (const Plane& sp : surfPlanes)
			{
				if (poly.size() < 3) break;
				ClipPoly(poly, sp, tmp);
			}
			if (poly.size() < 3) continue;
			Vector3 center(0, 0, 0);
			for (const ClipVert& cv : poly) center = center + cv.p;
			center = center * (1.0 / poly.size());
			// Nudge inward off the cut plane so the parity ray doesn't graze the surface.
			if (!InsideMesh(center - pl.n * 1e-4, soup)) continue;
			for (size_t k = 2; k < poly.size(); ++k)
				pushTri(poly[0], poly[k - 1], poly[k]);
		}

		if (piece.verts.size() < 9 * 2) continue;   // a lone sliver triangle is not a piece
		Vector3 pmn(1e30, 1e30, 1e30), pmx(-1e30, -1e30, -1e30), cen(0, 0, 0);
		const size_t vc = piece.verts.size() / 3;
		for (size_t k = 0; k < vc; ++k)
		{
			Vector3 p(piece.verts[k * 3], piece.verts[k * 3 + 1], piece.verts[k * 3 + 2]);
			cen = cen + p;
			pmn.x = std::min(pmn.x, p.x); pmx.x = std::max(pmx.x, p.x);
			pmn.y = std::min(pmn.y, p.y); pmx.y = std::max(pmx.y, p.y);
			pmn.z = std::min(pmn.z, p.z); pmx.z = std::max(pmx.z, p.z);
		}
		cen = cen * (1.0 / vc);
		for (size_t k = 0; k < vc; ++k)
		{
			piece.verts[k * 3]     -= (float)cen.x;
			piece.verts[k * 3 + 1] -= (float)cen.y;
			piece.verts[k * 3 + 2] -= (float)cen.z;
		}
		piece.centroid = cen;
		piece.halfExtents = (pmx - pmn) * 0.5;
		out.push_back(std::move(piece));
	}
	return !out.empty();
}

}  // namespace nuke
