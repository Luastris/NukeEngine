// Rope/chain component: capsule-segment chain over the physics seam + a per-frame generated
// visual (tube / repeated links / auto-rigged source mesh). See Rope.h.
#include "API/Model/Rope.h"
#include "API/Model/Atom.h"
#include "API/Model/Collider.h"
#include "API/Model/DebugDraw.h"
#include "API/Model/Events.h"
#include "API/Model/Game.h"
#include "API/Model/Material.h"
#include "API/Model/Mesh.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"
#include "interface/Services.h"
#include "service/iPhysics.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nuke {

static const float kD2R = 0.01745329252f;

static Vector3 Vc(const Vector3& a, const Vector3& b)
{ return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static double Vd(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vector3 Vn(const Vector3& v)
{
	const double l = std::sqrt(Vd(v, v));
	return l > 1e-12 ? Vector3(v.x / l, v.y / l, v.z / l) : Vector3(0, 1, 0);
}

// Shortest-arc rotation taking local +Y (the capsule axis) onto `dir`.
static Quaternion YTo(const Vector3& dir)
{
	Vector3 y(0, 1, 0);
	const double d = Vd(y, dir);
	if (d > 0.99999) return Quaternion::Identity();
	if (d < -0.99999) return Quaternion::FromAxisAngle(Vector3(1, 0, 0), 180.0);
	Vector3 ax = Vn(Vc(y, dir));
	return Quaternion::FromAxisAngle(ax, std::acos(std::max(-1.0, std::min(1.0, d))) / kD2R);
}

void Rope::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Rope::Destroy()
{
	Teardown();
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

void Rope::Reset() { Teardown(); }

void Rope::Teardown()
{
	iPhysics* ph = GetService<iPhysics>();
	if (ph)
	{
		if (startPin) ph->destroyJoint(startPin);
		if (endPin)   ph->destroyJoint(endPin);
		for (uint64_t l : links) if (l) ph->destroyJoint(l);
		for (const Seg& s : segs) if (s.body) ph->destroyBody(s.body);
	}
	startPin = endPin = 0;
	links.clear();
	segs.clear();
	built = false;
}

// The authored polyline in WORLD space, resampled to ~segmentLength nodes.
static void SampleAuthored(const std::vector<float>& pts, Transform* t, float segLen,
                           std::vector<Vector3>& out)
{
	out.clear();
	const int n = (int)pts.size() / 3;
	if (n < 2 || !t) return;
	Vector3 base = t->globalPosition();
	Quaternion rot = t->globalRotation();
	Vector3 scl = t->globalScale();
	std::vector<Vector3> w(n);
	for (int i = 0; i < n; ++i)
	{
		Vector3 lp(pts[i * 3] * scl.x, pts[i * 3 + 1] * scl.y, pts[i * 3 + 2] * scl.z);
		w[i] = base + rot.Rotate(lp);
	}
	double total = 0.0;
	for (int i = 1; i < n; ++i) { Vector3 d = w[i] - w[i - 1]; total += std::sqrt(Vd(d, d)); }
	if (total < 1e-4) return;
	const int nodes = std::max(2, (int)std::llround(total / std::max(0.05f, segLen)) + 1);
	const double step = total / (nodes - 1);
	out.push_back(w[0]);
	double walked = 0.0, want = step;
	for (int i = 1; i < n; ++i)
	{
		Vector3 d = w[i] - w[i - 1];
		double seg = std::sqrt(Vd(d, d));
		while (seg > 1e-9 && walked + seg >= want - 1e-9 && (int)out.size() < nodes - 1)
		{
			const double f = (want - walked) / seg;
			out.push_back(w[i - 1] + d * f);
			want += step;
		}
		walked += seg;
	}
	out.push_back(w[n - 1]);
}

uint64_t Rope::TieEnd(int segIndex, Atom* attach, const Vector3& worldPoint)
{
	iPhysics* ph = GetService<iPhysics>();
	if (!ph || segIndex < 0 || segIndex >= (int)segs.size()) return 0;
	uint64_t other = 0;
	if (attach)
	{
		Collider* c = attach->GetComponent<Collider>();
		if (!c || !c->bodyId) return 0;
		other = c->bodyId;
	}
	NukeConstraintDesc d;
	d.type = 2;   // distance, rigidly short = a ball-socket tie
	d.bodyA = other;
	d.bodyB = segs[segIndex].body;
	d.limit = true; d.min = 0.0f; d.max = 0.02f;
	d.pivot[0] = d.pivotB[0] = (float)worldPoint.x;
	d.pivot[1] = d.pivotB[1] = (float)worldPoint.y;
	d.pivot[2] = d.pivotB[2] = (float)worldPoint.z;
	return ph->createConstraint(d);
}

void Rope::Build()
{
	iPhysics* ph = GetService<iPhysics>();
	if (!ph) return;
	// An attach target names a body: wait until its Collider body exists.
	if (attachStart && (!attachStart->GetComponent<Collider>() || !attachStart->GetComponent<Collider>()->bodyId)) return;
	if (attachEnd && (!attachEnd->GetComponent<Collider>() || !attachEnd->GetComponent<Collider>()->bodyId)) return;

	std::vector<Vector3> nodes;
	SampleAuthored(points, transform, segmentLength, nodes);
	if (nodes.size() < 2) return;
	const int n = (int)nodes.size() - 1;

	segs.reserve(n);
	for (int i = 0; i < n; ++i)
	{
		Vector3 d = nodes[i + 1] - nodes[i];
		const double len = std::sqrt(Vd(d, d));
		Vector3 dir = Vn(d);
		Vector3 c = (nodes[i] + nodes[i + 1]) * 0.5;
		Quaternion q = YTo(dir);
		NukeBodyDesc bd;
		bd.shape = 2;   // capsule, axis = local Y
		bd.radius = radius;
		// Shrunk so neighbours only kiss at the joints instead of overlapping and jittering.
		bd.halfHeight = std::max(0.01f, (float)(len * 0.5) - radius);
		bd.motion = 1;
		bd.mass = std::max(0.01f, totalMass / n);
		bd.linearDamping = 0.05f;
		bd.angularDamping = 0.2f;
		bd.pos[0] = (float)c.x; bd.pos[1] = (float)c.y; bd.pos[2] = (float)c.z;
		bd.quat[0] = (float)q.x; bd.quat[1] = (float)q.y; bd.quat[2] = (float)q.z; bd.quat[3] = (float)q.w;
		Seg s;
		s.body = ph->createBody(bd);
		s.len = (float)len;
		if (!s.body) { Teardown(); return; }
		segs.push_back(s);
	}
	links.assign(std::max(0, n - 1), 0);
	for (int i = 0; i + 1 < n; ++i)
	{
		Vector3 a = Vn(nodes[i + 1] - nodes[i]);
		Vector3 b = Vn(nodes[i + 2] - nodes[i + 1]);
		Vector3 tw = Vn(a + b);
		Vector3 pl = Vc(tw, std::fabs(tw.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0));
		NukeJointDesc jd;
		jd.bodyA = segs[i].body;
		jd.bodyB = segs[i + 1].body;
		jd.pivot[0] = (float)nodes[i + 1].x; jd.pivot[1] = (float)nodes[i + 1].y; jd.pivot[2] = (float)nodes[i + 1].z;
		jd.twistAxis[0] = (float)tw.x; jd.twistAxis[1] = (float)tw.y; jd.twistAxis[2] = (float)tw.z;
		jd.planeAxis[0] = (float)pl.x; jd.planeAxis[1] = (float)pl.y; jd.planeAxis[2] = (float)pl.z;
		jd.twistMin = -twistLimit * kD2R;
		jd.twistMax =  twistLimit * kD2R;
		jd.swing1 = jd.swing2 = bendLimit * kD2R;
		links[i] = ph->createSwingTwistJoint(jd);
	}
	if (attachStart || pinStart) startPin = TieEnd(0, attachStart, nodes.front());
	if (attachEnd || pinEnd)     endPin   = TieEnd(n - 1, attachEnd, nodes.back());
	built = true;
}

void Rope::FixedUpdate()
{
	if (!atom || !Game::IsPlaying()) return;
	if (!built) Build();
}

// ---- runtime API --------------------------------------------------------------------------

bool   Rope::Built()        { return built; }
double Rope::SegmentCount() { return (double)segs.size(); }

static bool SegPose(iPhysics* ph, const Rope* r, uint64_t body, Vector3& c, Quaternion& q)
{
	(void)r;
	float p[3], quat[4];
	if (!ph || !body || !ph->getBodyPose(body, p, quat)) return false;
	c = Vector3(p[0], p[1], p[2]);
	q = Quaternion(quat[0], quat[1], quat[2], quat[3]);
	return true;
}

Vector3 Rope::SegmentPos(double i)
{
	const int idx = (int)i;
	if (idx < 0 || idx >= (int)segs.size()) return Vector3(0, 0, 0);
	Vector3 c; Quaternion q;
	return SegPose(GetService<iPhysics>(), this, segs[idx].body, c, q) ? c : Vector3(0, 0, 0);
}

Vector3 Rope::EndPos()
{
	if (segs.empty()) return Vector3(0, 0, 0);
	Vector3 c; Quaternion q;
	const Seg& s = segs.back();
	if (!SegPose(GetService<iPhysics>(), this, s.body, c, q)) return Vector3(0, 0, 0);
	return c + q.Rotate(Vector3(0, s.len * 0.5, 0));
}

double Rope::Length()
{
	double l = 0.0;
	for (const Seg& s : segs) l += s.len;
	return l;
}

bool Rope::Cut(const Vector3& worldPos)
{
	iPhysics* ph = GetService<iPhysics>();
	if (!ph || !built) return false;
	int best = -1;
	double bestD = 1e300;
	Vector3 wp = worldPos;
	for (int i = 0; i < (int)links.size(); ++i)
	{
		if (!links[i]) continue;
		Vector3 c; Quaternion q;
		if (!SegPose(ph, this, segs[i].body, c, q)) continue;
		Vector3 tip = c + q.Rotate(Vector3(0, segs[i].len * 0.5f, 0));
		Vector3 d = tip - wp;
		const double dd = Vd(d, d);
		if (dd < bestD) { bestD = dd; best = i; }
	}
	if (best < 0) return false;
	ph->destroyJoint(links[best]);
	links[best] = 0;
	nlohmann::json j;
	j["atom"] = (double)(atom ? atom->id.id : 0);
	j["link"] = (double)best;
	Events::Emit("rope.cut", j.dump());
	return true;
}

void Rope::Pull(const Vector3& impulse) { PullAt((double)segs.size() - 1.0, impulse); }

void Rope::PullAt(double seg, const Vector3& impulse)
{
	iPhysics* ph = GetService<iPhysics>();
	const int idx = (int)seg;
	if (!ph || idx < 0 || idx >= (int)segs.size()) return;
	const float v[3] = { (float)impulse.x, (float)impulse.y, (float)impulse.z };
	ph->addImpulse(segs[idx].body, v);
}

void Rope::AttachStartTo(Atom* a)
{
	iPhysics* ph = GetService<iPhysics>();
	if (!ph || segs.empty()) return;
	if (startPin) { ph->destroyJoint(startPin); startPin = 0; }
	if (!a && !pinStart) return;
	Vector3 c; Quaternion q;
	if (!SegPose(ph, this, segs.front().body, c, q)) return;
	startPin = TieEnd(0, a, c - q.Rotate(Vector3(0, segs.front().len * 0.5f, 0)));
}

void Rope::AttachEndTo(Atom* a)
{
	iPhysics* ph = GetService<iPhysics>();
	if (!ph || segs.empty()) return;
	if (endPin) { ph->destroyJoint(endPin); endPin = 0; }
	if (!a && !pinEnd) return;
	endPin = TieEnd((int)segs.size() - 1, a, EndPos());
}

// ---- visual -------------------------------------------------------------------------------

// Clone-swap the renderer's owned material instance (mirrors MeshRenderer::Init resolution).
static void ApplyRopeMat(MeshRenderer* mr, const std::string& guid)
{
	if (mr->mat) { Reflect_DropObject(mr->mat); delete mr->mat; mr->mat = nullptr; }
	if (!guid.empty())
		if (Material* asset = ResDB::getSingleton()->GetMaterial(guid))
			mr->mat = asset->Clone();
	mr->matGuid = guid;
}

void Rope::EnsureRenderer()
{
	if (mr || !atom) return;
	mr = new MeshRenderer();
	mr->transient = true;   // owner-managed; never serialized with the world
	atom->AddComponent(mr);
	ApplyRopeMat(mr, matGuid);
	matRes = matGuid;
}

void Rope::FreeGen()
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

void Rope::EnsureGen(int vertCount, bool uvs)
{
	if (gen && gen->numVerts == vertCount && ((gen->uvArray != nullptr) == uvs)) return;
	if (gen)
	{
		if (AppInstance* app = AppInstance::GetSingleton())
			if (app->render) app->render->invalidateMesh(gen);
		delete[] gen->vertexArray; delete[] gen->normalArray; delete[] gen->uvArray;
	}
	else
		gen = new Mesh();
	gen->vertexArray = new float[(size_t)vertCount * 3];
	gen->normalArray = new float[(size_t)vertCount * 3];
	gen->uvArray = uvs ? new float[(size_t)vertCount * 2] : nullptr;
	gen->numVerts = vertCount;
	gen->numIndices = 0;
	gen->indexArray = nullptr;
	std::snprintf(gen->name, sizeof(gen->name), "rope");
}

// Station chain: node per segment boundary with parallel-transport frames; `span` splits at
// cut links so the visual tears where the rope did.
void Rope::CollectStations(std::vector<Station>& out)
{
	out.clear();
	iPhysics* ph = GetService<iPhysics>();
	int span = 0;
	float dAcc = 0.0f;
	if (built && ph)
	{
		Vector3 prevN(1, 0, 0);
		bool first = true;
		for (int i = 0; i < (int)segs.size(); ++i)
		{
			Vector3 c; Quaternion q;
			if (!SegPose(ph, this, segs[i].body, c, q)) continue;
			Vector3 dir = Vn(q.Rotate(Vector3(0, 1, 0)));
			Vector3 a = c - dir * (segs[i].len * 0.5);
			Vector3 b = c + dir * (segs[i].len * 0.5);
			// Parallel transport: keep the previous normal, re-orthogonalized to this tangent.
			Vector3 nn = prevN - dir * Vd(prevN, dir);
			if (Vd(nn, nn) < 1e-10) nn = Vc(dir, std::fabs(dir.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0));
			nn = Vn(nn);
			prevN = nn;
			if (first) { out.push_back({ a, dir, nn, dAcc, span }); first = false; }
			dAcc += segs[i].len;
			out.push_back({ b, dir, nn, dAcc, span });
			if (i < (int)links.size() && !links[i]) { ++span; first = true; }
		}
	}
	else
	{
		// Edit-mode preview: the authored polyline.
		std::vector<Vector3> nodes;
		SampleAuthored(points, transform, segmentLength, nodes);
		Vector3 prevN(1, 0, 0);
		for (int i = 0; i < (int)nodes.size(); ++i)
		{
			Vector3 dir = Vn(i + 1 < (int)nodes.size() ? nodes[i + 1] - nodes[i]
			                                           : nodes[i] - nodes[i - 1]);
			Vector3 nn = prevN - dir * Vd(prevN, dir);
			if (Vd(nn, nn) < 1e-10) nn = Vc(dir, std::fabs(dir.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0));
			nn = Vn(nn);
			prevN = nn;
			if (i > 0) { Vector3 d = nodes[i] - nodes[i - 1]; dAcc += (float)std::sqrt(Vd(d, d)); }
			out.push_back({ nodes[i], dir, nn, dAcc, 0 });
		}
	}
}

// Catmull-Rom subdivision through the chain joints, per span — the tube/auto-rig looks stop
// showing the simulation segments. Frames re-transport along the refined polyline.
void Rope::RefineStations(std::vector<Station>& st)
{
	const int sub = std::max(0, std::min(8, smoothing));
	if (sub < 1 || st.size() < 3) return;
	std::vector<Station> out;
	out.reserve(st.size() * (sub + 1));
	auto cr = [](Vector3& p0, Vector3& p1, Vector3& p2, Vector3& p3, double t) -> Vector3
	{
		const double t2 = t * t, t3 = t2 * t;
		return (p1 * 2.0 + (p2 - p0) * t
		      + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * t2
		      + (p1 * 3.0 - p0 - p2 * 3.0 + p3) * t3) * 0.5;
	};
	size_t i = 0;
	while (i < st.size())
	{
		size_t j = i;
		while (j + 1 < st.size() && st[j + 1].span == st[i].span) ++j;   // [i..j] = one span
		if (j - i < 1) { out.push_back(st[i]); i = j + 1; continue; }
		for (size_t k = i; k < j; ++k)
		{
			Vector3 p0 = st[k > i ? k - 1 : k].p;
			Vector3 p1 = st[k].p;
			Vector3 p2 = st[k + 1].p;
			Vector3 p3 = st[k + 1 < j ? k + 2 : k + 1].p;
			for (int s = 0; s <= sub; ++s)
			{
				if (s == 0 && k > i) continue;   // shared with the previous cell
				Station n = st[k];
				n.p = cr(p0, p1, p2, p3, (double)s / (sub + 1));
				out.push_back(n);
			}
		}
		out.push_back(st[j]);
		i = j + 1;
	}
	// Re-derive tangents, arc length and parallel-transport normals on the refined chain.
	Vector3 prevN(1, 0, 0);
	float d = 0.0f;
	for (size_t k = 0; k < out.size(); ++k)
	{
		const bool spanStart = k == 0 || out[k].span != out[k - 1].span;
		const bool spanEnd = k + 1 >= out.size() || out[k + 1].span != out[k].span;
		Vector3 dir = out[k].t;
		if (!spanEnd)      dir = out[k + 1].p - out[k].p;
		else if (!spanStart) dir = out[k].p - out[k - 1].p;
		dir = Vn(dir);
		if (spanStart) { prevN = out[k].n; if (k > 0) d = out[k].d; }
		else { Vector3 dd = out[k].p - out[k - 1].p; d += (float)std::sqrt(Vd(dd, dd)); }
		Vector3 nn = prevN - dir * Vd(prevN, dir);
		if (Vd(nn, nn) < 1e-10) nn = Vc(dir, std::fabs(dir.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0));
		nn = Vn(nn);
		prevN = nn;
		out[k].t = dir;
		out[k].n = nn;
		out[k].d = d;
	}
	st.swap(out);
}

void Rope::OnRender(iRender*, RenderPhase phase)
{
	if (phase != RenderPhase::Overlay || !atom) return;   // once per frame, edit and play alike
	if (visual == 3) { if (mr) FreeGen(); return; }
	if (mr && matRes != matGuid) { ApplyRopeMat(mr, matGuid); matRes = matGuid; }
	BuildVisual();
}

void Rope::BuildVisual()
{
	std::vector<Station> st;
	CollectStations(st);
	if (st.size() < 2) { if (mr) FreeGen(); return; }
	if (visual == 0 || visual == 2) RefineStations(st);   // links keep the rigid chain look

	if (visual == 0)
	{
		// Tube: a ring per station, quads (as a soup) between same-span neighbours.
		const int sides = std::max(3, std::min(24, tubeSides));
		int quads = 0;
		for (size_t i = 0; i + 1 < st.size(); ++i)
			if (st[i].span == st[i + 1].span) quads += sides;
		if (!quads) { if (mr) FreeGen(); return; }
		EnsureGen(quads * 6, true);
		float* pv = gen->vertexArray;
		float* pn = gen->normalArray;
		float* pu = gen->uvArray;
		size_t o = 0;
		auto ring = [&](Station& s, int k, Vector3& pos, Vector3& nrm)
		{
			const double a = (double)k / sides * 6.283185307179586;
			Vector3 bn = Vn(Vc(s.n, s.t));
			nrm = s.n * std::cos(a) + bn * std::sin(a);
			pos = s.p + nrm * radius;
		};
		for (size_t i = 0; i + 1 < st.size(); ++i)
		{
			if (st[i].span != st[i + 1].span) continue;
			for (int k = 0; k < sides; ++k)
			{
				Vector3 p00, n00, p01, n01, p10, n10, p11, n11;
				ring(st[i], k, p00, n00);
				ring(st[i], k + 1, p01, n01);
				ring(st[i + 1], k, p10, n10);
				ring(st[i + 1], k + 1, p11, n11);
				const float u0 = (float)k / sides, u1 = (float)(k + 1) / sides;
				const float v0 = st[i].d, v1 = st[i + 1].d;
				const Vector3 P[6] = { p00, p10, p11, p00, p11, p01 };
				const Vector3 N[6] = { n00, n10, n11, n00, n11, n01 };
				const float U[6] = { u0, u0, u1, u0, u1, u1 };
				const float V[6] = { v0, v1, v1, v0, v1, v0 };
				for (int t = 0; t < 6; ++t, ++o)
				{
					pv[o * 3] = (float)P[t].x; pv[o * 3 + 1] = (float)P[t].y; pv[o * 3 + 2] = (float)P[t].z;
					pn[o * 3] = (float)N[t].x; pn[o * 3 + 1] = (float)N[t].y; pn[o * 3 + 2] = (float)N[t].z;
					pu[o * 2] = U[t]; pu[o * 2 + 1] = V[t];
				}
			}
		}
	}
	else
	{
		Mesh* src = meshGuid.empty() ? nullptr : ResDB::getSingleton()->GetMesh(meshGuid);
		const int tris = src ? src->TriCount() : 0;
		if (!src || !src->vertexArray || tris <= 0) { if (mr) FreeGen(); return; }
		const int fA = std::min(std::max(meshAxis, 0), 2);
		const int uA = (fA == 1) ? 2 : 1;
		const int rA = 3 - fA - uA;
		float mnf = FLT_MAX, mxf = -FLT_MAX;
		for (int t = 0; t < tris; ++t)
			for (int k = 0; k < 3; ++k)
			{
				const float v = src->vertexArray[(size_t)src->TriIndex(t, k) * 3 + fA];
				mnf = std::min(mnf, v); mxf = std::max(mxf, v);
			}
		const double srcLen = (double)(mxf - mnf);
		if (srcLen < 1e-6) { if (mr) FreeGen(); return; }

		// Frame at an arc distance (same span only), lerped between stations.
		auto frameAt = [&](double dist, int wantSpan, Vector3& C, Vector3& T, Vector3& N) -> bool
		{
			for (size_t i = 0; i + 1 < st.size(); ++i)
			{
				if (st[i].span != st[i + 1].span || st[i + 1].span != wantSpan) continue;
				if (dist > st[i + 1].d + 1e-6) continue;
				const double f = (st[i + 1].d - st[i].d) > 1e-9
				               ? std::max(0.0, (dist - st[i].d) / (st[i + 1].d - st[i].d)) : 0.0;
				C = st[i].p + (st[i + 1].p - st[i].p) * f;
				T = Vn(st[i].t + (st[i + 1].t - st[i].t) * f);
				N = Vn(st[i].n + (st[i + 1].n - st[i].n) * f);
				return true;
			}
			return false;
		};

		if (visual == 1)
		{
			// Links: one rigid source copy per segment, twisted per link (chains: 90 deg).
			const int count = built ? (int)segs.size() : (int)st.size() - 1;
			EnsureGen(tris * 3 * count, src->uvArray != nullptr);
			float* pv = gen->vertexArray; float* pn = gen->normalArray; float* pu = gen->uvArray;
			size_t o = 0;
			int li = 0;
			for (size_t i = 0; i + 1 < st.size(); ++i)
			{
				if (st[i].span != st[i + 1].span) continue;
				Station& A = st[i];
				Station& B = st[i + 1];
				Vector3 C = (A.p + B.p) * 0.5;
				Vector3 T = Vn(B.p - A.p);
				const double tw = (double)linkTwist * li * kD2R;
				Vector3 bn0 = Vn(Vc(A.n, T));
				Vector3 N = Vn(A.n * std::cos(tw) + bn0 * std::sin(tw));
				N = Vn(N - T * Vd(N, T));
				Vector3 B3 = Vn(Vc(N, T));
				Vector3 d = B.p - A.p;
				const double scl = std::sqrt(Vd(d, d)) / srcLen;
				for (int t = 0; t < tris; ++t)
					for (int k = 0; k < 3; ++k, ++o)
					{
						const uint32_t idx = src->TriIndex(t, k);
						const float* v = src->vertexArray + (size_t)idx * 3;
						const double vf = ((double)v[fA] - mnf - srcLen * 0.5) * scl;
						const double vr = (double)v[rA] * scl;
						const double vu = (double)v[uA] * scl;
						pv[o * 3]     = (float)(C.x + B3.x * vr + N.x * vu + T.x * vf);
						pv[o * 3 + 1] = (float)(C.y + B3.y * vr + N.y * vu + T.y * vf);
						pv[o * 3 + 2] = (float)(C.z + B3.z * vr + N.z * vu + T.z * vf);
						Vector3 wn(0, 1, 0);
						if (src->normalArray)
						{
							const float* sn = src->normalArray + (size_t)idx * 3;
							wn = Vn(Vector3(B3.x * sn[rA] + N.x * sn[uA] + T.x * sn[fA],
							                B3.y * sn[rA] + N.y * sn[uA] + T.y * sn[fA],
							                B3.z * sn[rA] + N.z * sn[uA] + T.z * sn[fA]));
						}
						pn[o * 3] = (float)wn.x; pn[o * 3 + 1] = (float)wn.y; pn[o * 3 + 2] = (float)wn.z;
						if (pu) { pu[o * 2] = src->uvArray[(size_t)idx * 2]; pu[o * 2 + 1] = src->uvArray[(size_t)idx * 2 + 1]; }
					}
				++li;
			}
		}
		else
		{
			// Auto-rig: the source mesh bends along the chain — its long axis maps onto the arc.
			const double L = st.back().d;
			if (L < 1e-6) { if (mr) FreeGen(); return; }
			EnsureGen(tris * 3, src->uvArray != nullptr);
			float* pv = gen->vertexArray; float* pn = gen->normalArray; float* pu = gen->uvArray;
			size_t o = 0;
			for (int t = 0; t < tris; ++t)
				for (int k = 0; k < 3; ++k, ++o)
				{
					const uint32_t idx = src->TriIndex(t, k);
					const float* v = src->vertexArray + (size_t)idx * 3;
					const double dist = ((double)v[fA] - mnf) / srcLen * L;
					Vector3 C, T, N;
					int span = 0;
					for (size_t i = 0; i + 1 < st.size(); ++i)
						if (dist >= st[i].d - 1e-6 && dist <= st[i + 1].d + 1e-6 && st[i].span == st[i + 1].span)
						{ span = st[i].span; break; }
					if (!frameAt(dist, span, C, T, N)) { C = st.back().p; T = st.back().t; N = st.back().n; }
					Vector3 B3 = Vn(Vc(N, T));
					pv[o * 3]     = (float)(C.x + B3.x * v[rA] + N.x * v[uA]);
					pv[o * 3 + 1] = (float)(C.y + B3.y * v[rA] + N.y * v[uA]);
					pv[o * 3 + 2] = (float)(C.z + B3.z * v[rA] + N.z * v[uA]);
					Vector3 wn(0, 1, 0);
					if (src->normalArray)
					{
						const float* sn = src->normalArray + (size_t)idx * 3;
						wn = Vn(Vector3(B3.x * sn[rA] + N.x * sn[uA] + T.x * sn[fA],
						                B3.y * sn[rA] + N.y * sn[uA] + T.y * sn[fA],
						                B3.z * sn[rA] + N.z * sn[uA] + T.z * sn[fA]));
					}
					pn[o * 3] = (float)wn.x; pn[o * 3 + 1] = (float)wn.y; pn[o * 3 + 2] = (float)wn.z;
					if (pu) { pu[o * 2] = src->uvArray[(size_t)idx * 2]; pu[o * 2 + 1] = src->uvArray[(size_t)idx * 2 + 1]; }
				}
		}
	}

	++gen->version;
	gen->boundsValid = false;
	EnsureRenderer();
	if (mr) mr->mesh = gen;
}

}  // namespace nuke
