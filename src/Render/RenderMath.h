// Matrices in Direct3D's convention, as the original engine uses them:
// row vectors (v' = v * M), left-handed coordinates (x right, y up, z
// forward), clip-space depth 0..1. Stored row-major, which is the memory
// layout a GLSL column-major mat4 needs to compute M^T * v, i.e. the same
// transform, so matrices upload unchanged and shaders write `M * v`.
#pragma once

#include <array>
#include <cmath>

namespace dice::hfe::render {

struct Vec3f {
	float x = 0, y = 0, z = 0;

	Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
};

inline float dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross(const Vec3f& a, const Vec3f& b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3f normalize(const Vec3f& v)
{
	const float length = std::sqrt(dot(v, v));
	return length > 0 ? v * (1 / length) : v;
}

struct Mat4 {
	std::array<float, 16> m {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

	float& at(int row, int column) { return m[row * 4 + column]; }
	float at(int row, int column) const { return m[row * 4 + column]; }

	Mat4 operator*(const Mat4& o) const
	{
		Mat4 r;
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				float sum = 0;
				for (int k = 0; k < 4; ++k) {
					sum += at(i, k) * o.at(k, j);
				}
				r.at(i, j) = sum;
			}
		}
		return r;
	}

	static Mat4 translation(const Vec3f& t)
	{
		Mat4 r;
		r.at(3, 0) = t.x;
		r.at(3, 1) = t.y;
		r.at(3, 2) = t.z;
		return r;
	}

	static Mat4 rotationX(float radians)
	{
		const float c = std::cos(radians), s = std::sin(radians);
		Mat4 r;
		r.at(1, 1) = c;
		r.at(1, 2) = s;
		r.at(2, 1) = -s;
		r.at(2, 2) = c;
		return r;
	}

	static Mat4 rotationY(float radians)
	{
		const float c = std::cos(radians), s = std::sin(radians);
		Mat4 r;
		r.at(0, 0) = c;
		r.at(0, 2) = -s;
		r.at(2, 0) = s;
		r.at(2, 2) = c;
		return r;
	}

	static Mat4 rotationZ(float radians)
	{
		const float c = std::cos(radians), s = std::sin(radians);
		Mat4 r;
		r.at(0, 0) = c;
		r.at(0, 1) = s;
		r.at(1, 0) = -s;
		r.at(1, 1) = c;
		return r;
	}

	// The engine's "yaw/pitch/roll" rotations in degrees, like
	// D3DXMatrixRotationYawPitchRoll: roll about z, then pitch about x,
	// then yaw about y.
	static Mat4 yawPitchRoll(float yawDegrees, float pitchDegrees, float rollDegrees)
	{
		constexpr float toRadians = 3.14159265358979f / 180.0f;
		return rotationZ(rollDegrees * toRadians) * rotationX(pitchDegrees * toRadians) *
			rotationY(yawDegrees * toRadians);
	}

	static Mat4 lookAt(const Vec3f& eye, const Vec3f& target, const Vec3f& up)
	{
		const Vec3f zAxis = normalize(target - eye);
		const Vec3f xAxis = normalize(cross(up, zAxis));
		const Vec3f yAxis = cross(zAxis, xAxis);
		Mat4 r;
		r.m = {xAxis.x, yAxis.x, zAxis.x, 0, xAxis.y, yAxis.y, zAxis.y, 0, xAxis.z, yAxis.z, zAxis.z, 0,
			-dot(xAxis, eye), -dot(yAxis, eye), -dot(zAxis, eye), 1};
		return r;
	}

	static Mat4 perspective(float fovYRadians, float aspect, float zNear, float zFar)
	{
		const float yScale = 1 / std::tan(fovYRadians / 2);
		const float xScale = yScale / aspect;
		Mat4 r;
		r.m = {xScale, 0, 0, 0, 0, yScale, 0, 0, 0, 0, zFar / (zFar - zNear), 1, 0, 0, -zNear * zFar / (zFar - zNear), 0};
		return r;
	}
};

} // namespace dice::hfe::render
