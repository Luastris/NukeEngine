#include "API/Model/DebugDraw.h"
#include "interface/AppInstance.h"
#include "render/irender.h"
#include <cmath>

namespace nuke {

static const int kCircleSegs = 32;

static void Emit(const Vector3& a, const Vector3& b, const Color& c)
{
	iRender* r = AppInstance::GetSingleton()->render;
	if (!r) return;
	float fa[3] = { (float)a.x, (float)a.y, (float)a.z };
	float fb[3] = { (float)b.x, (float)b.y, (float)b.z };
	float fc[4] = { (float)c.r, (float)c.g, (float)c.b, (float)c.a };
	r->drawDebugLine(fa, fb, fc);
}

void DebugDraw::Line(const Vector3& a, const Vector3& b, const Color& color) { Emit(a, b, color); }

// Orthonormal basis around `dir` (any perpendicular pair).
static void Basis(const Vector3& dir, Vector3& u, Vector3& v)
{
	Vector3 d = dir;
	double len = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (len < 1e-9) { u = Vector3(1, 0, 0); v = Vector3(0, 0, 1); return; }
	d.x /= len; d.y /= len; d.z /= len;
	Vector3 up = fabs(d.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	u = Vector3(d.y * up.z - d.z * up.y, d.z * up.x - d.x * up.z, d.x * up.y - d.y * up.x);   // d x up
	double ul = sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
	u.x /= ul; u.y /= ul; u.z /= ul;
	v = Vector3(d.y * u.z - d.z * u.y, d.z * u.x - d.x * u.z, d.x * u.y - d.y * u.x);         // d x u
}

void DebugDraw::Arrow(const Vector3& from, const Vector3& to, const Color& color)
{
	Emit(from, to, color);
	Vector3 dir(to.x - from.x, to.y - from.y, to.z - from.z);
	double len = sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (len < 1e-9) return;
	const double head = len * 0.15;
	Vector3 d(dir.x / len, dir.y / len, dir.z / len);
	Vector3 u, v; Basis(d, u, v);
	Vector3 base(to.x - d.x * head, to.y - d.y * head, to.z - d.z * head);
	const double w = head * 0.5;
	Emit(to, Vector3(base.x + u.x * w, base.y + u.y * w, base.z + u.z * w), color);
	Emit(to, Vector3(base.x - u.x * w, base.y - u.y * w, base.z - u.z * w), color);
	Emit(to, Vector3(base.x + v.x * w, base.y + v.y * w, base.z + v.z * w), color);
	Emit(to, Vector3(base.x - v.x * w, base.y - v.y * w, base.z - v.z * w), color);
}

void DebugDraw::WireBox(const Vector3& center, const Vector3& half, const Quaternion& rot, const Color& color)
{
	Quaternion q = rot.Normalized();
	Vector3 corners[8];
	for (int i = 0; i < 8; ++i)
	{
		Vector3 local((i & 1) ? half.x : -half.x,
		              (i & 2) ? half.y : -half.y,
		              (i & 4) ? half.z : -half.z);
		Vector3 wpos = q.Rotate(local);
		corners[i] = Vector3(center.x + wpos.x, center.y + wpos.y, center.z + wpos.z);
	}
	static const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
	for (auto& e : edges) Emit(corners[e[0]], corners[e[1]], color);
}

void DebugDraw::WireCircle(const Vector3& center, const Vector3& normal, double radius, const Color& color)
{
	Vector3 u, v; Basis(normal, u, v);
	Vector3 prev;
	for (int i = 0; i <= kCircleSegs; ++i)
	{
		const double a = (2.0 * 3.14159265358979 * i) / kCircleSegs;
		Vector3 p(center.x + (u.x * cos(a) + v.x * sin(a)) * radius,
		          center.y + (u.y * cos(a) + v.y * sin(a)) * radius,
		          center.z + (u.z * cos(a) + v.z * sin(a)) * radius);
		if (i) Emit(prev, p, color);
		prev = p;
	}
}

void DebugDraw::WireSphere(const Vector3& center, double radius, const Color& color)
{
	WireCircle(center, Vector3(1, 0, 0), radius, color);
	WireCircle(center, Vector3(0, 1, 0), radius, color);
	WireCircle(center, Vector3(0, 0, 1), radius, color);
}

void DebugDraw::WireCapsule(const Vector3& center, double radius, double halfHeight,
                            const Quaternion& rot, const Color& color)
{
	Quaternion q = rot.Normalized();
	Vector3 axis = q.Rotate(Vector3(0, 1, 0));
	Vector3 top(center.x + axis.x * halfHeight, center.y + axis.y * halfHeight, center.z + axis.z * halfHeight);
	Vector3 bot(center.x - axis.x * halfHeight, center.y - axis.y * halfHeight, center.z - axis.z * halfHeight);
	WireCircle(top, axis, radius, color);
	WireCircle(bot, axis, radius, color);
	WireSphere(top, radius, color);   // hemispheres approximated by full spheres (gizmo fidelity)
	WireSphere(bot, radius, color);
	Vector3 u, v; Basis(axis, u, v);
	for (const Vector3& s : { u, v })
	{
		Emit(Vector3(top.x + s.x * radius, top.y + s.y * radius, top.z + s.z * radius),
		     Vector3(bot.x + s.x * radius, bot.y + s.y * radius, bot.z + s.z * radius), color);
		Emit(Vector3(top.x - s.x * radius, top.y - s.y * radius, top.z - s.z * radius),
		     Vector3(bot.x - s.x * radius, bot.y - s.y * radius, bot.z - s.z * radius), color);
	}
}

void DebugDraw::WireCone(const Vector3& apex, const Vector3& dir, double angleDeg, double range, const Color& color)
{
	Vector3 d = dir;
	double len = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (len < 1e-9 || range <= 0.0) return;
	d.x /= len; d.y /= len; d.z /= len;
	const double r = range * tan(angleDeg * 3.14159265358979 / 180.0);
	Vector3 base(apex.x + d.x * range, apex.y + d.y * range, apex.z + d.z * range);
	WireCircle(base, d, r, color);
	Vector3 u, v; Basis(d, u, v);
	Emit(apex, Vector3(base.x + u.x * r, base.y + u.y * r, base.z + u.z * r), color);
	Emit(apex, Vector3(base.x - u.x * r, base.y - u.y * r, base.z - u.z * r), color);
	Emit(apex, Vector3(base.x + v.x * r, base.y + v.y * r, base.z + v.z * r), color);
	Emit(apex, Vector3(base.x - v.x * r, base.y - v.y * r, base.z - v.z * r), color);
}

}  // namespace nuke
