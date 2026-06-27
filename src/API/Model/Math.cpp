#include "API/Model/Math.h"

namespace nuke {

double Math::Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double Math::Clamp01(double v) { return Clamp(v, 0.0, 1.0); }

double Math::LerpUnclamped(double a, double b, double t) { return a + (b - a) * t; }
double Math::Lerp(double a, double b, double t)          { return LerpUnclamped(a, b, Clamp01(t)); }

Vector3 Math::LerpUnclamped(const Vector3& a, const Vector3& b, double t)
{
	return Vector3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}
Vector3 Math::Lerp(const Vector3& a, const Vector3& b, double t)
{
	return LerpUnclamped(a, b, Clamp01(t));
}

}  // namespace nuke
