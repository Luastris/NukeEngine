#pragma once
#ifndef NUKEE_MATH_H
#define NUKEE_MATH_H
#include "NukeAPI.h"
#include "Vector.h"
#include <cmath>
#include <algorithm>

namespace nuke {

// ---- volumes and the atom's scale ------------------------------------------------------------
// Every volume prop (collider extents, zone radius, probe box, decal size) is authored in the
// atom's LOCAL units. Scaling the atom must scale what it affects — a component that reads its
// props raw keeps its authored size while the object visibly grows, which is exactly the class
// of bug where a hovering object still touched the water. These two calls are the single rule.
inline Vector3 ScaleExtents(const Vector3& localHalf, const Vector3& worldScale)
{
	return Vector3(localHalf.x * std::fabs(worldScale.x),
	               localHalf.y * std::fabs(worldScale.y),
	               localHalf.z * std::fabs(worldScale.z));
}
// A sphere/capsule radius cannot follow three axes at once: the largest one wins, so the volume
// never ends up smaller than the geometry it stands for.
inline float ScaleRadius(float localRadius, const Vector3& worldScale)
{
	const double m = std::max(std::fabs(worldScale.x), std::max(std::fabs(worldScale.y), std::fabs(worldScale.z)));
	return (float)(localRadius * m);
}


// Engine math helpers (Unity Mathf-style). Add general numeric/vector utilities here.
class NUKEENGINE_API Math
{
public:
	static double  Clamp(double v, double lo, double hi);
	static double  Clamp01(double v);

	// Linear interpolation. Lerp clamps t to [0,1]; LerpUnclamped extrapolates.
	static double  Lerp(double a, double b, double t);
	static double  LerpUnclamped(double a, double b, double t);
	static Vector3 Lerp(const Vector3& a, const Vector3& b, double t);
	static Vector3 LerpUnclamped(const Vector3& a, const Vector3& b, double t);
};

}  // namespace nuke

#endif
