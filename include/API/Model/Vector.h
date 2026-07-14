#pragma once
#ifndef NUKEE_VECTOR_H
#define NUKEE_VECTOR_H
#include "NukeAPI.h"
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/ext.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>

namespace nuke {

class NUKEENGINE_API Vector2
{
public:
	double x, y;

	Vector2();
	Vector2(double x, double y);

	double abs();
	Vector2 normalize();

	Vector2 operator+(Vector2 other);
	Vector2 operator+=(Vector2 other);
	Vector2 operator-(Vector2 other);
	Vector2 operator-=(Vector2 other);
	Vector2 operator*(Vector2 other);
	Vector2 operator*=(Vector2 other);
	Vector2 operator*(double other);
	Vector2 operator*=(double other);
	Vector2 operator/(Vector2 other);
	Vector2 operator/=(Vector2 other);
	Vector2 operator/(double other);
	Vector2 operator/=(double other);

	std::string toStringA();
	std::wstring toStringW();

    class Vector3 toVector3();
    class Vector4 toVector4();
};

class NUKEENGINE_API Vector3 : public Vector2
{
public:
	double z;

	Vector3();
	Vector3(double x, double y, double z);

	static const Vector3 forward; 
	static const Vector3 backward;
	static const Vector3 up;
	static const Vector3 left;
	static const Vector3 right;
	static const Vector3 down;
	static const Vector3 zero;
	static const Vector3 one;

	double abs();
	Vector3 normalize();

	Vector3 operator+(Vector2 other);
	Vector3 operator+(Vector3 other);
	Vector3 operator+=(Vector2 other);
	Vector3 operator+=(Vector3 other);
	Vector3 operator-(Vector2 other);
	Vector3 operator-(Vector3 other);
	Vector3 operator-=(Vector2 other);
	Vector3 operator-=(Vector3 other);
	Vector3 operator*(Vector3 other);
	Vector3 operator*=(Vector3 other);
	Vector3 operator/(Vector3 other);
	Vector3 operator/=(Vector3 other);
	Vector3 operator* (double other) const;
	Vector3 operator*=(double other);
	Vector3 operator/(double other);
	Vector3 operator/=(double other);

	std::string toStringA();
	std::wstring toStringW();

    class Vector4 toVector4();
};


class NUKEENGINE_API Vector4 : public Vector3
{
public:
	double w;

	Vector4();
	Vector4(double x, double y, double z, double w);
	
	double abs();
	Vector4 normalize();

	Vector4 operator+(Vector2 other);
	Vector4 operator+(Vector3 other);
	Vector4 operator+(Vector4 other);
	Vector4 operator+=(Vector2 other);
	Vector4 operator+=(Vector3 other);
	Vector4 operator+=(Vector4 other);
	Vector4 operator-(Vector2 other);
	Vector4 operator-(Vector3 other);
	Vector4 operator-(Vector4 other);
	Vector4 operator-=(Vector2 other);
	Vector4 operator-=(Vector3 other);
	Vector4 operator-=(Vector4 other);
	Vector4 operator*(Vector4 other);
	Vector4 operator*=(Vector4 other);
	Vector4 operator/(Vector4 other);
	Vector4 operator/=(Vector4 other);
	Vector4 operator*(double other);
	Vector4 operator*=(double other);
	Vector4 operator/(double other);
	Vector4 operator/=(double other);

	std::string toStringA();
	std::wstring toStringW();

    class Color toColor();
};

class NUKEENGINE_API Quaternion : public Vector4
{
public:
	Quaternion();
	Quaternion(double x, double y, double z, double w);

	Quaternion operator = (const Quaternion& q);
	Quaternion operator + (const Quaternion& q);
	Quaternion operator - (const Quaternion& q);
	Quaternion operator * (const Quaternion& q);
	Quaternion operator / (Quaternion& q);
	Quaternion operator += (const Quaternion& q);
	Quaternion operator -= (const Quaternion& q);
	Quaternion operator *= (const Quaternion& q);
	Quaternion operator /= (Quaternion& q);
	bool  operator != (const Quaternion& q);
	bool  operator == (const Quaternion& q);
	Quaternion scale(double  s);
	Quaternion inverse();
	Quaternion conjugate();
	double norm();
	double magnitude();
	Quaternion UnitQuaternion();

	// ---- Rotation utilities ------------------------------------------------------------
	// Conventions match Transform (glm-backed): angles in DEGREES, euler order XYZ,
	// forward = +Z (Transform::direction() == Rotate({0,0,1})).

	static Quaternion Identity();
	static Quaternion FromEulerDeg(const Vector3& deg);            // inverse of ToEulerDeg
	Vector3           ToEulerDeg() const;
	static Quaternion FromAxisAngle(const Vector3& axis, double deg);
	// Rotation that looks along `forward` with `up` as the up-hint (both world-space;
	// forward must be non-zero, up must not be parallel to it).
	static Quaternion LookRotation(const Vector3& forward, const Vector3& up = Vector3(0, 1, 0));
	static double     Dot(const Quaternion& a, const Quaternion& b);
	// Spherical interpolation along the SHORT arc; t clamped to [0,1].
	static Quaternion Slerp(const Quaternion& a, const Quaternion& b, double t);
	Vector3           Rotate(const Vector3& v) const;              // rotate a vector by this rotation
	Quaternion        Normalized() const;                          // unit quaternion (identity if zero)
};


}  // namespace nuke

#endif // !NUKEE_VECTOR_H

