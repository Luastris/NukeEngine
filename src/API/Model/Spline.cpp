#include "API/Model/Spline.h"
#include "API/Model/Atom.h"
#include "API/Model/DebugDraw.h"
#include "API/Model/Math.h"
#include "API/Model/Mesh.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Material.h"
#include "API/Model/Time.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"
#include "reflect/ReflectBind.h"
#include <render/irender.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <random>

namespace nuke {

// ---- small vector helpers (float-precision curve math) -----------------------------------------
static inline Vector3 Vc(const Vector3& a, const Vector3& b)
{ return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline double Vd(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline double Vl(const Vector3& a) { return std::sqrt(Vd(a, a)); }
static inline Vector3 Vn(const Vector3& a)
{ const double l = Vl(a); return l > 1e-12 ? Vector3(a.x / l, a.y / l, a.z / l) : Vector3(0, 0, 1); }

static uint64_t FnvMix(uint64_t h, const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
	return h;
}
static constexpr uint64_t kFnv = 1469598103934665603ull;

// Cubic-Hermite curve keys (t, value, inTan, outTan) — the house curve-widget layout.
static float EvalCurveKeys(const std::vector<float>& c, float u)
{
	const size_t n = c.size() / 4;
	if (!n) return 0.0f;
	if (n == 1 || u <= c[0]) return c[1];
	if (u >= c[(n - 1) * 4]) return c[(n - 1) * 4 + 1];
	size_t k = 1; while (k < n && u > c[k * 4]) ++k;
	const float t0 = c[(k - 1) * 4], v0 = c[(k - 1) * 4 + 1];
	const float t1 = c[k * 4],       v1 = c[k * 4 + 1];
	const float h = t1 - t0; if (h < 1e-6f) return v1;
	const float m0 = c[(k - 1) * 4 + 3] * h, m1 = c[k * 4 + 2] * h;
	const float x = (u - t0) / h, x2 = x * x, x3 = x2 * x;
	return (2 * x3 - 3 * x2 + 1) * v0 + (x3 - 2 * x2 + x) * m0 + (-2 * x3 + 3 * x2) * v1 + (x3 - x2) * m1;
}

// ---- Spline ------------------------------------------------------------------------------------

Spline::Spline() : Component("Spline") {}
void Spline::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}
void Spline::Destroy() {}
void Spline::Update() {}
void Spline::FixedUpdate() {}
void Spline::Pause() {}
void Spline::Reset() { sampleStamp = ~0ull; worldStamp = ~0ull; }

uint64_t Spline::Stamp()
{
	uint64_t h = kFnv;
	if (!points.empty()) h = FnvMix(h, points.data(), points.size() * sizeof(float));
	h = FnvMix(h, &type, sizeof(type));
	h = FnvMix(h, &closed, sizeof(closed));
	return h;
}

// Resample the control curve into the local station table: adaptive per-segment step count,
// central-difference tangents, parallel-transport normals (no twist flips along the way).
void Spline::RebuildLocal()
{
	samples.clear();
	const int n = (int)points.size() / 3;
	if (n < 2) return;
	auto pt = [&](int i)
	{
		i = std::min(std::max(i, 0), n - 1);
		return Vector3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2]);
	};
	auto ptw = [&](int i) { return pt(((i % n) + n) % n); };

	std::vector<Vector3> poly;
	auto emit = [&](const Vector3& v)
	{
		if (!poly.empty())
		{
			const Vector3& b = poly.back();
			const double dx = v.x - b.x, dy = v.y - b.y, dz = v.z - b.z;
			if (dx * dx + dy * dy + dz * dz < 1e-10) return;   // drop duplicate stations
		}
		poly.push_back(v);
	};
	auto emitCubic = [&](const Vector3& A, const Vector3& h1, const Vector3& h2, const Vector3& B)
	{
		const double net = Vl(Vector3(h1.x - A.x, h1.y - A.y, h1.z - A.z)) +
		                   Vl(Vector3(h2.x - h1.x, h2.y - h1.y, h2.z - h1.z)) +
		                   Vl(Vector3(B.x - h2.x, B.y - h2.y, B.z - h2.z));
		const int steps = std::max(8, (int)std::ceil(net * 4.0));   // ~25cm stations
		for (int k = 0; k < steps; ++k)
		{
			const double u = (double)k / steps, w = 1.0 - u;
			const double b0 = w * w * w, b1 = 3.0 * w * w * u, b2 = 3.0 * w * u * u, b3 = u * u * u;
			emit(Vector3(b0 * A.x + b1 * h1.x + b2 * h2.x + b3 * B.x,
			             b0 * A.y + b1 * h1.y + b2 * h2.y + b3 * B.y,
			             b0 * A.z + b1 * h1.z + b2 * h2.z + b3 * B.z));
		}
	};
	auto emitCatmull = [&](const Vector3& p0, const Vector3& p1, const Vector3& p2, const Vector3& p3)
	{
		const double chord = Vl(Vector3(p2.x - p1.x, p2.y - p1.y, p2.z - p1.z));
		const int steps = std::max(8, (int)std::ceil(chord * 4.0));
		for (int k = 0; k < steps; ++k)
		{
			const double u = (double)k / steps, u2 = u * u, u3 = u2 * u;
			auto cr = [&](double a, double b, double c, double d)
			{ return 0.5 * ((2 * b) + (-a + c) * u + (2 * a - 5 * b + 4 * c - d) * u2 + (-a + 3 * b - 3 * c + d) * u3); };
			emit(Vector3(cr(p0.x, p1.x, p2.x, p3.x), cr(p0.y, p1.y, p2.y, p3.y), cr(p0.z, p1.z, p2.z, p3.z)));
		}
	};

	if (type == 1)
	{
		// Bezier: anchors at 0,3,6,... — an incomplete tail (missing handles) is ignored.
		const int segs = (n - 1) / 3;
		for (int s = 0; s < segs; ++s)
			emitCubic(pt(s * 3), pt(s * 3 + 1), pt(s * 3 + 2), pt(s * 3 + 3));
		if (segs == 0) { emit(pt(0)); emit(pt(n - 1)); }   // no full cubic yet: a straight span
		if (closed && n >= 4)
		{
			// Wrap span back to the first anchor; trailing handle pair if the layout carries
			// one (count % 3 == 0), auto thirds otherwise.
			const Vector3 A = pt(segs * 3), B = pt(0);
			Vector3 h1, h2;
			if (n >= segs * 3 + 3)
			{ h1 = pt(segs * 3 + 1); h2 = pt(segs * 3 + 2); }
			else
			{
				h1 = Vector3(A.x + (B.x - A.x) / 3.0, A.y + (B.y - A.y) / 3.0, A.z + (B.z - A.z) / 3.0);
				h2 = Vector3(A.x + (B.x - A.x) * 2.0 / 3.0, A.y + (B.y - A.y) * 2.0 / 3.0, A.z + (B.z - A.z) * 2.0 / 3.0);
			}
			emitCubic(A, h1, h2, B);
			emit(pt(0));
		}
		else
			emit(pt(segs * 3 > 0 ? segs * 3 : n - 1));
	}
	else
	{
		const int segs = closed ? n : n - 1;
		for (int s = 0; s < segs; ++s)
		{
			if (closed) emitCatmull(ptw(s - 1), ptw(s), ptw(s + 1), ptw(s + 2));
			else        emitCatmull(pt(s - 1), pt(s), pt(s + 1), pt(s + 2));
		}
		emit(closed ? pt(0) : pt(n - 1));
	}

	const int m = (int)poly.size();
	if (m < 2) { samples.clear(); return; }
	samples.resize(m);
	double d = 0.0;
	Vector3 nrm(0, 1, 0);
	for (int i = 0; i < m; ++i)
	{
		const Vector3& p = poly[i];
		if (i > 0) d += Vl(Vector3(p.x - poly[i - 1].x, p.y - poly[i - 1].y, p.z - poly[i - 1].z));
		// Tangent: central difference (one-sided at the ends; closed curves wrap through the
		// duplicated first/last station, which keeps the seam smooth enough for framing).
		const Vector3& pa = poly[std::max(i - 1, 0)];
		const Vector3& pb = poly[std::min(i + 1, m - 1)];
		Vector3 t = Vn(Vector3(pb.x - pa.x, pb.y - pa.y, pb.z - pa.z));
		if (i == 0)
		{
			// Initial normal: world-up projected off the tangent (fallback +X when vertical).
			Vector3 ref = std::fabs(t.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
			nrm = Vn(Vector3(ref.x - t.x * Vd(t, ref), ref.y - t.y * Vd(t, ref), ref.z - t.z * Vd(t, ref)));
		}
		else
		{
			// Parallel transport: keep the previous normal, re-orthogonalized to the new tangent.
			Vector3 c = Vector3(nrm.x - t.x * Vd(t, nrm), nrm.y - t.y * Vd(t, nrm), nrm.z - t.z * Vd(t, nrm));
			if (Vl(c) > 1e-6) nrm = Vn(c);
		}
		SplineSample& s = samples[i];
		s.p[0] = (float)p.x;  s.p[1] = (float)p.y;  s.p[2] = (float)p.z;
		s.t[0] = (float)t.x;  s.t[1] = (float)t.y;  s.t[2] = (float)t.z;
		s.n[0] = (float)nrm.x; s.n[1] = (float)nrm.y; s.n[2] = (float)nrm.z;
		s.d = (float)d;
	}
}

const std::vector<SplineSample>& Spline::LocalSamples()
{
	const uint64_t st = Stamp();
	if (st != sampleStamp) { sampleStamp = st; worldStamp = ~0ull; RebuildLocal(); }
	return samples;
}

double Spline::LocalLength()
{
	const std::vector<SplineSample>& s = LocalSamples();
	return s.empty() ? 0.0 : (double)s.back().d;
}

void Spline::FrameAtLocal(double dist, Vector3& pos, Vector3& tan, Vector3& nrm)
{
	const std::vector<SplineSample>& s = LocalSamples();
	if (s.size() < 2)
	{
		pos = Vector3(0, 0, 0); tan = Vector3(0, 0, 1); nrm = Vector3(0, 1, 0);
		if (!s.empty()) pos = Vector3(s[0].p[0], s[0].p[1], s[0].p[2]);
		return;
	}
	const int m = (int)s.size();
	dist = Math::Clamp(dist, 0.0, (double)s.back().d);
	int lo = 0, hi = m - 1;
	while (hi - lo > 1) { const int mid = (lo + hi) / 2; (s[mid].d <= dist ? lo : hi) = mid; }
	const SplineSample& a = s[lo];
	const SplineSample& b = s[hi];
	const double seg = (double)b.d - a.d;
	const double f = seg > 1e-9 ? (dist - a.d) / seg : 0.0;
	pos = Vector3(a.p[0] + (b.p[0] - a.p[0]) * f, a.p[1] + (b.p[1] - a.p[1]) * f, a.p[2] + (b.p[2] - a.p[2]) * f);
	tan = Vn(Vector3(a.t[0] + (b.t[0] - a.t[0]) * f, a.t[1] + (b.t[1] - a.t[1]) * f, a.t[2] + (b.t[2] - a.t[2]) * f));
	nrm = Vn(Vector3(a.n[0] + (b.n[0] - a.n[0]) * f, a.n[1] + (b.n[1] - a.n[1]) * f, a.n[2] + (b.n[2] - a.n[2]) * f));
}

Vector3 Spline::ToWorld(const float p[3]) const
{
	if (!transform) return Vector3(p[0], p[1], p[2]);
	Transform* t = const_cast<Transform*>((const Transform*)transform);
	const Vector3 P = t->globalPosition();
	const Quaternion Q = t->globalRotation();
	const Vector3 S = t->globalScale();
	const Vector3 r = Q.Rotate(Vector3(p[0] * S.x, p[1] * S.y, p[2] * S.z));
	return Vector3(P.x + r.x, P.y + r.y, P.z + r.z);
}

// Refresh the world arc-length table when the atom moved/scaled (rotation and translation keep
// lengths; non-uniform scale does not, so lengths are measured on the transformed stations).
void Spline::EnsureWorld()
{
	const std::vector<SplineSample>& s = LocalSamples();
	uint64_t h = FnvMix(kFnv, &sampleStamp, sizeof(sampleStamp));
	if (transform)
	{
		const Vector3 P = transform->globalPosition();
		const Quaternion Q = transform->globalRotation();
		const Vector3 S = transform->globalScale();
		const double d[10] = { P.x, P.y, P.z, Q.x, Q.y, Q.z, Q.w, S.x, S.y, S.z };
		h = FnvMix(h, d, sizeof(d));
	}
	if (h == worldStamp) return;
	worldStamp = h;
	worldD.assign(s.size(), 0.0f);
	worldLen = 0.0;
	for (size_t i = 1; i < s.size(); ++i)
	{
		const Vector3 a = ToWorld(s[i - 1].p), b = ToWorld(s[i].p);
		worldLen += Vl(Vector3(b.x - a.x, b.y - a.y, b.z - a.z));
		worldD[i] = (float)worldLen;
	}
}

double Spline::Length() { EnsureWorld(); return worldLen; }

Vector3 Spline::PositionAt(double dist)
{
	EnsureWorld();
	const std::vector<SplineSample>& s = samples;
	if (s.empty()) return transform ? transform->globalPosition() : Vector3(0, 0, 0);
	if (s.size() < 2) return ToWorld(s[0].p);
	dist = Math::Clamp(dist, 0.0, worldLen);
	int lo = 0, hi = (int)s.size() - 1;
	while (hi - lo > 1) { const int mid = (lo + hi) / 2; ((double)worldD[mid] <= dist ? lo : hi) = mid; }
	const double seg = (double)worldD[hi] - worldD[lo];
	const double f = seg > 1e-9 ? (dist - worldD[lo]) / seg : 0.0;
	const Vector3 a = ToWorld(s[lo].p), b = ToWorld(s[hi].p);
	return Vector3(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f);
}

// World direction of a cached local unit vector (non-uniform scale shears, so re-normalize).
static Vector3 DirToWorld(Transform* t, const Vector3& v)
{
	if (!t) return v;
	const Quaternion Q = t->globalRotation();
	const Vector3 S = t->globalScale();
	return Vn(Q.Rotate(Vector3(v.x * S.x, v.y * S.y, v.z * S.z)));
}

Vector3 Spline::TangentAt(double dist)
{
	EnsureWorld();
	if (samples.size() < 2) return Vector3(0, 0, 1);
	// Map the world distance back to the matching station pair and blend their tangents.
	dist = Math::Clamp(dist, 0.0, worldLen);
	int lo = 0, hi = (int)samples.size() - 1;
	while (hi - lo > 1) { const int mid = (lo + hi) / 2; ((double)worldD[mid] <= dist ? lo : hi) = mid; }
	const double seg = (double)worldD[hi] - worldD[lo];
	const double f = seg > 1e-9 ? (dist - worldD[lo]) / seg : 0.0;
	const SplineSample& a = samples[lo];
	const SplineSample& b = samples[hi];
	return DirToWorld(transform, Vector3(a.t[0] + (b.t[0] - a.t[0]) * f, a.t[1] + (b.t[1] - a.t[1]) * f, a.t[2] + (b.t[2] - a.t[2]) * f));
}

Vector3 Spline::NormalAt(double dist)
{
	EnsureWorld();
	if (samples.size() < 2) return Vector3(0, 1, 0);
	dist = Math::Clamp(dist, 0.0, worldLen);
	int lo = 0, hi = (int)samples.size() - 1;
	while (hi - lo > 1) { const int mid = (lo + hi) / 2; ((double)worldD[mid] <= dist ? lo : hi) = mid; }
	const double seg = (double)worldD[hi] - worldD[lo];
	const double f = seg > 1e-9 ? (dist - worldD[lo]) / seg : 0.0;
	const SplineSample& a = samples[lo];
	const SplineSample& b = samples[hi];
	return DirToWorld(transform, Vector3(a.n[0] + (b.n[0] - a.n[0]) * f, a.n[1] + (b.n[1] - a.n[1]) * f, a.n[2] + (b.n[2] - a.n[2]) * f));
}

double Spline::ClosestDistance(const Vector3& wp)
{
	EnsureWorld();
	const std::vector<SplineSample>& s = samples;
	if (s.size() < 2) return 0.0;
	double bestD2 = 1e300, bestDist = 0.0;
	Vector3 a = ToWorld(s[0].p);
	for (size_t i = 1; i < s.size(); ++i)
	{
		const Vector3 b = ToWorld(s[i].p);
		const Vector3 ab(b.x - a.x, b.y - a.y, b.z - a.z);
		const double L2 = Vd(ab, ab);
		double t = L2 > 1e-12 ? Vd(Vector3(wp.x - a.x, wp.y - a.y, wp.z - a.z), ab) / L2 : 0.0;
		t = Math::Clamp(t, 0.0, 1.0);
		const Vector3 c(a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t);
		const double d2 = (wp.x - c.x) * (wp.x - c.x) + (wp.y - c.y) * (wp.y - c.y) + (wp.z - c.z) * (wp.z - c.z);
		if (d2 < bestD2)
		{
			bestD2 = d2;
			bestDist = (double)worldD[i - 1] + ((double)worldD[i] - worldD[i - 1]) * t;
		}
		a = b;
	}
	return bestDist;
}

int Spline::PointCount() { return (int)points.size() / 3; }
Vector3 Spline::GetPoint(int i)
{
	if (i < 0 || i * 3 + 2 >= (int)points.size()) return Vector3(0, 0, 0);
	return Vector3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2]);
}
void Spline::SetPoint(int i, const Vector3& lp)
{
	if (i < 0 || i * 3 + 2 >= (int)points.size()) return;
	points[i * 3] = (float)lp.x; points[i * 3 + 1] = (float)lp.y; points[i * 3 + 2] = (float)lp.z;
}
void Spline::AddPoint(const Vector3& lp)
{
	if (type == 1 && points.size() >= 3)
	{
		// Bezier keeps the anchor,handle,handle,anchor layout: auto handles at the thirds
		// between the previous anchor and the new one.
		const int n = (int)points.size() / 3;
		const int lastA = ((n - 1) / 3) * 3;
		const Vector3 A(points[lastA * 3], points[lastA * 3 + 1], points[lastA * 3 + 2]);
		const Vector3 h1(A.x + (lp.x - A.x) / 3.0, A.y + (lp.y - A.y) / 3.0, A.z + (lp.z - A.z) / 3.0);
		const Vector3 h2(A.x + (lp.x - A.x) * 2.0 / 3.0, A.y + (lp.y - A.y) * 2.0 / 3.0, A.z + (lp.z - A.z) * 2.0 / 3.0);
		const float add[9] = { (float)h1.x, (float)h1.y, (float)h1.z,
		                       (float)h2.x, (float)h2.y, (float)h2.z,
		                       (float)lp.x, (float)lp.y, (float)lp.z };
		points.insert(points.end(), add, add + 9);
		return;
	}
	points.push_back((float)lp.x); points.push_back((float)lp.y); points.push_back((float)lp.z);
}
void Spline::RemovePoint(int i)
{
	const int n = (int)points.size() / 3;
	if (i < 0 || i >= n) return;
	if (type == 1)
	{
		// Only anchors are removable, together with their handle group (structure stays valid).
		if (i % 3 != 0 || n < 5) return;
		int first = i == 0 ? 0 : i - 1;
		if (i == ((n - 1) / 3) * 3) first = i - 2;   // last anchor: drop its leading handles
		points.erase(points.begin() + first * 3, points.begin() + (first + 3) * 3);
		return;
	}
	if (n <= 2) return;
	points.erase(points.begin() + i * 3, points.begin() + i * 3 + 3);
}

void Spline::OnRender(iRender* r, RenderPhase phase)
{
	if (!r || !transform || phase != RenderPhase::Overlay) return;
	AppInstance* app = AppInstance::GetSingleton();
	if (!app->isEditor() || app->selectedInHieararchy != atom) return;
	const std::vector<SplineSample>& s = LocalSamples();
	const Color cc(0.35, 1.0, 0.55, 1.0);
	for (size_t i = 1; i < s.size(); ++i)
		DebugDraw::Line(ToWorld(s[i - 1].p), ToWorld(s[i].p), cc);
	const int n = (int)points.size() / 3;
	for (int i = 0; i < n; ++i)
	{
		float lp[3] = { points[i * 3], points[i * 3 + 1], points[i * 3 + 2] };
		const bool handle = type == 1 && (i % 3) != 0;
		DebugDraw::WireSphere(ToWorld(lp), handle ? 0.12 : 0.18,
		                      handle ? Color(0.4, 0.8, 1.0, 1.0) : Color(1.0, 0.85, 0.3, 1.0));
		if (handle)
		{
			// Handle leash to its anchor (the previous point for h1, the next for h2).
			const int a = (i % 3) == 1 ? i - 1 : i + 1;
			if (a >= 0 && a < n)
			{
				float ap[3] = { points[a * 3], points[a * 3 + 1], points[a * 3 + 2] };
				DebugDraw::Line(ToWorld(lp), ToWorld(ap), Color(0.5, 0.5, 0.5, 0.8));
			}
		}
	}
}

// ---- SplineMesh --------------------------------------------------------------------------------

SplineMesh::SplineMesh() : Component("SplineMesh") {}
void SplineMesh::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}
void SplineMesh::Update() {}
void SplineMesh::FixedUpdate() {}
void SplineMesh::Pause() {}
void SplineMesh::Reset() { genStamp = ~0ull; }

void SplineMesh::Destroy()
{
	if (mr)
	{
		// The transient renderer sits AFTER this component in the atom's list (created later),
		// so unlinking it mid-teardown is safe — the walk simply never reaches it.
		if (atom) atom->components.remove(mr);
		mr->Destroy();
		Reflect_DropObject(mr);
		delete mr; mr = nullptr;
	}
	FreeGen();
}

void SplineMesh::FreeGen()
{
	if (!gen) return;
	if (AppInstance* app = AppInstance::GetSingleton())
		if (app->render) app->render->invalidateMesh(gen);
	// Mesh has no destructor — the owner frees the streams.
	delete[] gen->vertexArray; delete[] gen->normalArray; delete[] gen->uvArray;
	delete[] gen->indexArray; delete[] gen->colorArray;
	delete gen; gen = nullptr;
	if (mr) mr->mesh = nullptr;
}

// Clone-swap the renderer's owned material instance (mirrors MeshRenderer::Init resolution).
static void ApplyGenMat(MeshRenderer* mr, const std::string& guid)
{
	if (mr->mat) { Reflect_DropObject(mr->mat); delete mr->mat; mr->mat = nullptr; }
	if (!guid.empty())
		if (Material* asset = ResDB::getSingleton()->GetMaterial(guid))
			mr->mat = asset->Clone();
	mr->matGuid = guid;
}

void SplineMesh::EnsureRenderer()
{
	if (mr || !atom) return;
	mr = new MeshRenderer();
	mr->transient = true;   // owner-managed; never serialized with the world
	atom->AddComponent(mr);
	ApplyGenMat(mr, matGuid);
	matRes = matGuid;
}

void SplineMesh::OnRender(iRender*, RenderPhase phase)
{
	if (phase != RenderPhase::Overlay) return;   // once per frame, edit mode and play alike
	if (mr && matRes != matGuid) { ApplyGenMat(mr, matGuid); matRes = matGuid; }
	Spline* sp = atom ? atom->GetComponent<Spline>() : nullptr;
	uint64_t h = kFnv;
	if (sp) { const uint64_t ss = sp->Stamp(); h = FnvMix(h, &ss, sizeof(ss)); }
	if (!meshGuid.empty()) h = FnvMix(h, meshGuid.data(), meshGuid.size());
	h = FnvMix(h, &mode, sizeof(mode));
	h = FnvMix(h, &axis, sizeof(axis));
	h = FnvMix(h, &scale, sizeof(scale));
	h = FnvMix(h, &spacing, sizeof(spacing));
	const double off[3] = { offset.x, offset.y, offset.z };
	h = FnvMix(h, off, sizeof(off));
	if (h != genStamp) { genStamp = h; Rebuild(); }
	else if (mr && gen && mr->mesh != gen) mr->mesh = gen;   // renderer Reset() nulled it
}

void SplineMesh::Rebuild()
{
	Spline* sp = atom ? atom->GetComponent<Spline>() : nullptr;
	Mesh* src = meshGuid.empty() ? nullptr : ResDB::getSingleton()->GetMesh(meshGuid);
	const int tris = src ? src->TriCount() : 0;
	if (!sp || !src || !src->vertexArray || tris <= 0 || sp->LocalSamples().size() < 2)
	{ FreeGen(); return; }

	// Axis mapping: forward runs along the curve, `up` follows the frame normal, the remaining
	// axis follows the binormal (right).
	const int fA = std::min(std::max(axis, 0), 2);
	const int uA = (fA == 1) ? 2 : 1;
	const int rA = 3 - fA - uA;
	float mnf = FLT_MAX, mxf = -FLT_MAX;
	for (int t = 0; t < tris; ++t)
		for (int k = 0; k < 3; ++k)
		{
			const float v = src->vertexArray[(size_t)src->TriIndex(t, k) * 3 + fA];
			mnf = std::min(mnf, v); mxf = std::max(mxf, v);
		}
	const double srcLen = (double)(mxf - mnf) * scale;
	const double L = sp->LocalLength();
	if (srcLen < 1e-6 || L < 1e-6) { FreeGen(); return; }

	int copies; double tileLen = 0.0, pitch = 0.0;
	if (mode == 0) { copies = std::max(1, (int)std::llround(L / srcLen)); tileLen = L / copies; }
	else { pitch = srcLen + (double)spacing; copies = std::max(1, (int)((L + (double)spacing + 1e-6) / pitch)); }

	const size_t outVerts = (size_t)tris * 3 * copies;
	float* pv = new float[outVerts * 3];
	float* pn = new float[outVerts * 3];
	float* pu = src->uvArray ? new float[outVerts * 2] : nullptr;
	size_t o = 0;
	for (int c = 0; c < copies; ++c)
	{
		Vector3 C, T, N, B;
		if (mode != 0)
		{
			sp->FrameAtLocal((double)c * pitch + srcLen * 0.5 + offset.z, C, T, N);
			B = Vc(N, T);
		}
		for (int t = 0; t < tris; ++t)
			for (int k = 0; k < 3; ++k, ++o)
			{
				const uint32_t idx = src->TriIndex(t, k);
				const float* v = src->vertexArray + (size_t)idx * 3;
				const double vf = (double)(v[fA] - mnf) * scale;
				const double vr = (double)v[rA] * scale + offset.x;
				const double vu = (double)v[uA] * scale + offset.y;
				double along = 0.0;
				if (mode == 0)
				{
					// Deform: the forward extent stretches over this copy's share of the curve.
					sp->FrameAtLocal((double)c * tileLen + vf / srcLen * tileLen + offset.z, C, T, N);
					B = Vc(N, T);
				}
				else
					along = vf - srcLen * 0.5;
				pv[o * 3 + 0] = (float)(C.x + B.x * vr + N.x * vu + T.x * along);
				pv[o * 3 + 1] = (float)(C.y + B.y * vr + N.y * vu + T.y * along);
				pv[o * 3 + 2] = (float)(C.z + B.z * vr + N.z * vu + T.z * along);
				double nx = 0, ny = 1, nz = 0;
				if (src->normalArray)
				{
					const float* sn = src->normalArray + (size_t)idx * 3;
					const Vector3 w(B.x * sn[rA] + N.x * sn[uA] + T.x * sn[fA],
					                B.y * sn[rA] + N.y * sn[uA] + T.y * sn[fA],
					                B.z * sn[rA] + N.z * sn[uA] + T.z * sn[fA]);
					const Vector3 wn = Vn(w);
					nx = wn.x; ny = wn.y; nz = wn.z;
				}
				pn[o * 3 + 0] = (float)nx; pn[o * 3 + 1] = (float)ny; pn[o * 3 + 2] = (float)nz;
				if (pu) { pu[o * 2] = src->uvArray[(size_t)idx * 2]; pu[o * 2 + 1] = src->uvArray[(size_t)idx * 2 + 1]; }
			}
	}

	FreeGen();
	gen = new Mesh();
	std::snprintf(gen->name, sizeof(gen->name), "spline-mesh");
	gen->vertexArray = pv;
	gen->normalArray = pn;
	gen->uvArray = pu;
	gen->numVerts = (int)outVerts;
	gen->numIndices = 0;
	gen->indexArray = nullptr;
	++gen->version;
	gen->boundsValid = false;
	EnsureRenderer();
	if (mr) mr->mesh = gen;
}

// ---- SplineScatter -----------------------------------------------------------------------------

SplineScatter::SplineScatter() { name = (char*)"SplineScatter"; }

void SplineScatter::Rebuild()
{
	EnsureDecoded();
	instances.clear();
	Spline* sp = atom ? atom->GetComponent<Spline>() : nullptr;
	const double L = sp ? sp->LocalLength() : 0.0;
	if (sp && L > 1e-6 && spacing > 1e-4f)
	{
		std::mt19937 rng((uint32_t)seed);
		std::uniform_real_distribution<double> u01(0.0, 1.0);
		for (double d = 0.0; d <= L + 1e-6; d += (double)spacing)
		{
			const double dj = Math::Clamp(d + (u01(rng) - 0.5) * (double)jitter * spacing, 0.0, L);
			Vector3 C, T, N;
			sp->FrameAtLocal(dj, C, T, N);
			const Vector3 B = Vc(N, T);
			const double lat = (u01(rng) * 2.0 - 1.0) * (double)lateralSpread + offset.x;
			const Vector3 p(C.x + B.x * lat + N.x * offset.y + T.x * offset.z,
			                C.y + B.y * lat + N.y * offset.y + T.y * offset.z,
			                C.z + B.z * lat + N.z * offset.y + T.z * offset.z);
			Quaternion q = Quaternion::Slerp(Quaternion::Identity(), Quaternion::LookRotation(T, N),
			                                 Math::Clamp01(alignToCurve));
			if (randomYaw) q = q * Quaternion::FromAxisAngle(Vector3(0, 1, 0), u01(rng) * 360.0);
			const float sc = (float)(scaleMin + (scaleMax - scaleMin) * u01(rng));
			Inst in{};
			in.pos[0] = (float)p.x; in.pos[1] = (float)p.y; in.pos[2] = (float)p.z;
			in.quat[0] = (float)q.x; in.quat[1] = (float)q.y; in.quat[2] = (float)q.z; in.quat[3] = (float)q.w;
			in.scale[0] = in.scale[1] = in.scale[2] = sc;
			in.color[0] = in.color[1] = in.color[2] = in.color[3] = 1.0f;
			instances.push_back(in);
		}
	}
	decoded = true;   // authored live: the serialized blob is stale until the next save packs it
	MarkDirty();
}

bool SplineScatter::EnsureRenderReady(iRender* r)
{
	Spline* sp = atom ? atom->GetComponent<Spline>() : nullptr;
	uint64_t h = kFnv;
	if (sp) { const uint64_t ss = sp->Stamp(); h = FnvMix(h, &ss, sizeof(ss)); }
	const float f[6] = { spacing, jitter, lateralSpread, scaleMin, scaleMax, alignToCurve };
	h = FnvMix(h, f, sizeof(f));
	const double off[3] = { offset.x, offset.y, offset.z };
	h = FnvMix(h, off, sizeof(off));
	h = FnvMix(h, &seed, sizeof(seed));
	h = FnvMix(h, &randomYaw, sizeof(randomYaw));
	if (h != scatterStamp) { scatterStamp = h; Rebuild(); }
	return InstancedMesh::EnsureRenderReady(r);
}

// ---- SplineMover -------------------------------------------------------------------------------

SplineMover::SplineMover() : Component("SplineMover") {}
void SplineMover::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}
void SplineMover::Destroy() {}
void SplineMover::FixedUpdate() {}
void SplineMover::Pause() {}
void SplineMover::Reset() { dist = 0.0; dir = 1; playing = false; started = false; }

Spline* SplineMover::Resolve()
{
	Atom* a = splineAtom ? splineAtom : atom;
	return a ? a->GetComponent<Spline>() : nullptr;
}

void SplineMover::Play() { playing = true; started = true; }
void SplineMover::Stop() { playing = false; started = true; }
void SplineMover::SetProgress(double t01)
{
	if (Spline* sp = Resolve()) dist = Math::Clamp01(t01) * sp->Length();
}
double SplineMover::GetProgress()
{
	Spline* sp = Resolve();
	const double L = sp ? sp->Length() : 0.0;
	return L > 1e-9 ? Math::Clamp01(dist / L) : 0.0;
}

void SplineMover::Update()
{
	if (!started) { started = true; playing = playOnStart; }
	if (!playing || !transform) return;
	Spline* sp = Resolve();
	if (!sp) return;
	const double L = sp->Length();
	if (L < 1e-6) return;
	const double mul = speedCurve.empty() ? 1.0
	                 : (double)EvalCurveKeys(speedCurve, (float)Math::Clamp01(dist / L));
	dist += (double)speed * mul * dir * Time::getSingleton()->gameDelta;
	if (mode == 0)
	{ dist = std::fmod(dist, L); if (dist < 0.0) dist += L; }
	else if (mode == 1)
	{
		if (dist > L) { dist = 2.0 * L - dist; dir = -dir; }
		else if (dist < 0.0) { dist = -dist; dir = -dir; }
		dist = Math::Clamp(dist, 0.0, L);
	}
	else
	{
		if (dist >= L) { dist = L; playing = false; }
		if (dist < 0.0) dist = 0.0;
	}
	const Vector3 p = sp->PositionAt(dist);
	Quaternion rot = transform->globalRotation();
	if (align)
	{
		Vector3 t = sp->TangentAt(dist);
		if (dir < 0) t = Vector3(-t.x, -t.y, -t.z);
		rot = Quaternion::LookRotation(t, sp->NormalAt(dist));
	}
	transform->SetGlobal(p, rot, transform->globalScale());
}

}  // namespace nuke
