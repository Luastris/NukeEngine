#include "API/Model/Mesh.h"
#include <algorithm>
#include <cmath>
#include <vector>

// CPU ray queries against a mesh. The tree is a plain median-split BVH over triangle centroids:
// built once per mesh, walked front-to-back, and thrown away only when the vertices change.
// Everything is LOCAL space — callers transform the ray, not the geometry.

namespace nuke {

struct Mesh::RayTree
{
	struct Node
	{
		float mn[3], mx[3];
		int   first = 0, count = 0;   // leaf range into `order`; count = 0 -> inner node
		int   left = -1, right = -1;
	};
	std::vector<Node> nodes;
	std::vector<int>  order;          // triangle indices, permuted by the build
};

namespace {

inline void TriBounds(Mesh& m, int tri, float mn[3], float mx[3], float ctr[3])
{
	const uint32_t i0 = m.TriIndex(tri, 0), i1 = m.TriIndex(tri, 1), i2 = m.TriIndex(tri, 2);
	const float* p[3] = { m.vertexArray + (size_t)i0 * 3,
	                      m.vertexArray + (size_t)i1 * 3,
	                      m.vertexArray + (size_t)i2 * 3 };
	for (int k = 0; k < 3; ++k)
	{
		mn[k] = std::min(p[0][k], std::min(p[1][k], p[2][k]));
		mx[k] = std::max(p[0][k], std::max(p[1][k], p[2][k]));
		ctr[k] = (p[0][k] + p[1][k] + p[2][k]) * (1.0f / 3.0f);
	}
}

// Slab test; returns the entry distance so the walk can visit the nearer child first.
inline bool SlabHit(const float mn[3], const float mx[3], const float ro[3], const float invD[3],
                    float maxT, float& tEnter)
{
	float t0 = 0.0f, t1 = maxT;
	for (int k = 0; k < 3; ++k)
	{
		float ta = (mn[k] - ro[k]) * invD[k];
		float tb = (mx[k] - ro[k]) * invD[k];
		if (ta > tb) std::swap(ta, tb);
		t0 = std::max(t0, ta);
		t1 = std::min(t1, tb);
		if (t0 > t1) return false;
	}
	tEnter = t0;
	return true;
}

int BuildNode(Mesh& m, Mesh::RayTree& tree, std::vector<float>& ctr, int first, int count, int depth)
{
	const int self = (int)tree.nodes.size();
	tree.nodes.push_back(Mesh::RayTree::Node{});
	float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (int i = first; i < first + count; ++i)
	{
		float tmn[3], tmx[3], tc[3];
		TriBounds(m, tree.order[i], tmn, tmx, tc);
		for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], tmn[k]); mx[k] = std::max(mx[k], tmx[k]); }
	}
	Mesh::RayTree::Node& n = tree.nodes[self];
	for (int k = 0; k < 3; ++k) { n.mn[k] = mn[k]; n.mx[k] = mx[k]; }

	if (count <= 8 || depth > 32)   // leaf: a handful of triangles is cheaper tested directly
	{
		n.first = first; n.count = count;
		return self;
	}
	// Split on the widest axis at the median centroid — cheap to build, good enough to walk.
	int axis = 0;
	float ext[3] = { mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] };
	if (ext[1] > ext[axis]) axis = 1;
	if (ext[2] > ext[axis]) axis = 2;
	const int mid = first + count / 2;
	std::nth_element(tree.order.begin() + first, tree.order.begin() + mid, tree.order.begin() + first + count,
	                 [&](int a, int b) { return ctr[(size_t)a * 3 + axis] < ctr[(size_t)b * 3 + axis]; });
	const int l = BuildNode(m, tree, ctr, first, mid - first, depth + 1);
	const int r = BuildNode(m, tree, ctr, mid, first + count - mid, depth + 1);
	tree.nodes[self].left = l;
	tree.nodes[self].right = r;
	tree.nodes[self].count = 0;
	return self;
}

}  // namespace

void Mesh::InvalidateRayTree()
{
	delete rayTree;
	rayTree = nullptr;
}

bool Mesh::RaycastLocal(const float ro[3], const float rd[3], float maxT, RayHit& out)
{
	if (!vertexArray || numVerts < 3) return false;
	const int tris = TriCount();
	if (tris <= 0) return false;

	if (!rayTree)
	{
		RayTree* t = new RayTree();
		t->order.resize(tris);
		for (int i = 0; i < tris; ++i) t->order[i] = i;
		std::vector<float> ctr((size_t)tris * 3);
		for (int i = 0; i < tris; ++i)
		{
			float mn[3], mx[3], c[3];
			TriBounds(*this, i, mn, mx, c);
			ctr[(size_t)i * 3 + 0] = c[0]; ctr[(size_t)i * 3 + 1] = c[1]; ctr[(size_t)i * 3 + 2] = c[2];
		}
		t->nodes.reserve((size_t)tris / 4 + 8);
		BuildNode(*this, *t, ctr, 0, tris, 0);
		rayTree = t;
	}

	float invD[3];
	for (int k = 0; k < 3; ++k)
		invD[k] = (std::fabs(rd[k]) < 1e-9f) ? (rd[k] < 0.0f ? -1e30f : 1e30f) : 1.0f / rd[k];

	bool hit = false;
	float best = maxT;
	int   stack[64];
	int   sp = 0;
	stack[sp++] = 0;
	while (sp > 0)
	{
		const RayTree::Node& n = rayTree->nodes[stack[--sp]];
		float tEnter;
		if (!SlabHit(n.mn, n.mx, ro, invD, best, tEnter)) continue;
		if (n.count == 0)
		{
			if (n.left  >= 0 && sp < 63) stack[sp++] = n.left;
			if (n.right >= 0 && sp < 63) stack[sp++] = n.right;
			continue;
		}
		for (int i = n.first; i < n.first + n.count; ++i)
		{
			const int tri = rayTree->order[i];
			const uint32_t i0 = TriIndex(tri, 0), i1 = TriIndex(tri, 1), i2 = TriIndex(tri, 2);
			const float* a = vertexArray + (size_t)i0 * 3;
			const float* b = vertexArray + (size_t)i1 * 3;
			const float* c = vertexArray + (size_t)i2 * 3;
			// Möller-Trumbore
			const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			const float pv[3] = { rd[1] * e2[2] - rd[2] * e2[1],
			                      rd[2] * e2[0] - rd[0] * e2[2],
			                      rd[0] * e2[1] - rd[1] * e2[0] };
			const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
			if (std::fabs(det) < 1e-9f) continue;
			const float inv = 1.0f / det;
			const float tv[3] = { ro[0] - a[0], ro[1] - a[1], ro[2] - a[2] };
			const float bu = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
			if (bu < 0.0f || bu > 1.0f) continue;
			const float qv[3] = { tv[1] * e1[2] - tv[2] * e1[1],
			                      tv[2] * e1[0] - tv[0] * e1[2],
			                      tv[0] * e1[1] - tv[1] * e1[0] };
			const float bv = (rd[0] * qv[0] + rd[1] * qv[1] + rd[2] * qv[2]) * inv;
			if (bv < 0.0f || bu + bv > 1.0f) continue;
			const float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
			if (t < 1e-5f || t >= best) continue;
			best = t;
			hit = true;
			out.t = t;
			out.tri = tri;
			for (int k = 0; k < 3; ++k) out.point[k] = ro[k] + rd[k] * t;
			float n0[3] = { e1[1] * e2[2] - e1[2] * e2[1],
			                e1[2] * e2[0] - e1[0] * e2[2],
			                e1[0] * e2[1] - e1[1] * e2[0] };
			const float len = std::sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]);
			if (len > 1e-9f) { n0[0] /= len; n0[1] /= len; n0[2] /= len; }
			else             { n0[0] = 0; n0[1] = 1; n0[2] = 0; }
			if (n0[0] * rd[0] + n0[1] * rd[1] + n0[2] * rd[2] > 0.0f)
			{ n0[0] = -n0[0]; n0[1] = -n0[1]; n0[2] = -n0[2]; }
			for (int k = 0; k < 3; ++k) out.normal[k] = n0[k];
			out.u = out.v = -1.0f;
			if (uvArray)
			{
				const float* ua = uvArray + (size_t)i0 * 2;
				const float* ub = uvArray + (size_t)i1 * 2;
				const float* uc = uvArray + (size_t)i2 * 2;
				const float w0 = 1.0f - bu - bv;
				out.u = ua[0] * w0 + ub[0] * bu + uc[0] * bv;
				out.v = ua[1] * w0 + ub[1] * bu + uc[1] * bv;
			}
		}
	}
	return hit;
}

}  // namespace nuke
