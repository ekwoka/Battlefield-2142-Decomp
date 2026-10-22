// Minimal vector/matrix types matching the game's memory layout: Vec3 is
// three floats and Matrix4 is row-major with the translation in row 3, so the
// game can pass its own matrices and vectors straight through.
#pragma once

#include <cmath>

namespace dice::hfe::sound {

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	Vec3() = default;
	constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

	Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
	Vec3 operator-() const { return {-x, -y, -z}; }
	float lengthSquared() const { return x * x + y * y + z * z; }
	float length() const { return std::sqrt(lengthSquared()); }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 normalized(const Vec3& v)
{
	const float len = v.length();
	return len > 0.0f ? v * (1.0f / len) : v;
}
inline bool isFinite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Matrix4 {
	float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

	Vec3 right() const { return {m[0][0], m[0][1], m[0][2]}; }
	Vec3 up() const { return {m[1][0], m[1][1], m[1][2]}; }
	Vec3 forward() const { return {m[2][0], m[2][1], m[2][2]}; }
	Vec3 translation() const { return {m[3][0], m[3][1], m[3][2]}; }

	Vec3 transformPoint(const Vec3& v) const
	{
		return {v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0],
			v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1],
			v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + m[3][2]};
	}
	Vec3 transformVector(const Vec3& v) const
	{
		return {v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0], v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1],
			v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2]};
	}
};

// Rigid-body inverse (rotation transposed, translation negated).
inline Matrix4 inverseRigid(const Matrix4& a)
{
	Matrix4 r;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			r.m[i][j] = a.m[j][i];
		}
	}
	const Vec3 t = a.translation();
	r.m[3][0] = -dot(t, a.right());
	r.m[3][1] = -dot(t, a.up());
	r.m[3][2] = -dot(t, a.forward());
	return r;
}

} // namespace dice::hfe::sound
