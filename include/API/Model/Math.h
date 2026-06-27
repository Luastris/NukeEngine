#pragma once
#ifndef NUKEE_MATH_H
#define NUKEE_MATH_H
#include "NukeAPI.h"
#include "Vector.h"

namespace nuke {

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
