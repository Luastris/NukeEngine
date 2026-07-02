#pragma once
#ifndef NUKEE_DEBUGDRAW_H
#define NUKEE_DEBUGDRAW_H
#include "NukeAPI.h"
#include "Vector.h"
#include "Color.h"
#include "reflect/Reflect.h"

namespace nuke {

// Debug/gizmo drawing facade (roadmap 2.1). Shapes decompose ENGINE-side into
// iRender::drawDebugLine segments — backends only ever see lines. Everything is valid
// for ONE frame (the renderer clears at the next frame's start); emit every frame you
// want it visible (Update / fixedUpdate / editor gizmo pass — the seam is thread-safe).
//
// [[nuke::func]]-reflected: scripts get nuke.DebugDraw.* through the generic static
// binder with zero hand-written glue.
class NUKEENGINE_API DebugDraw
{
	NUKE_CLASS_NOCREATE(DebugDraw, Object)
public:
	[[nuke::func]] static void Line(const Vector3& a, const Vector3& b, const Color& color);
	[[nuke::func]] static void Arrow(const Vector3& from, const Vector3& to, const Color& color);
	[[nuke::func]] static void WireBox(const Vector3& center, const Vector3& halfExtents,
	                                   const Quaternion& rot, const Color& color);
	[[nuke::func]] static void WireSphere(const Vector3& center, double radius, const Color& color);
	[[nuke::func]] static void WireCapsule(const Vector3& center, double radius, double halfHeight,
	                                       const Quaternion& rot, const Color& color);
	// Cone from `apex` opening along `dir`: `angleDeg` = half-angle, `range` = height.
	[[nuke::func]] static void WireCone(const Vector3& apex, const Vector3& dir,
	                                    double angleDeg, double range, const Color& color);
	[[nuke::func]] static void WireCircle(const Vector3& center, const Vector3& normal,
	                                      double radius, const Color& color);
};

}  // namespace nuke

#endif // !NUKEE_DEBUGDRAW_H
