// The shaders' shared math compiled as C++, for the tests: the .glsl files
// below are written in the part of GLSL that C++ also reads, with the few
// types and built-ins they use mirrored here, so a test evaluates the very
// code the GPU runs (fog_share.glsl, particle_color.glsl).
#pragma once

#include <algorithm>

namespace dice::hfe::render::glsl {

struct vec3 {
	float x = 0, y = 0, z = 0;
	vec3() = default;
	vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct vec4 {
	float x = 0, y = 0, z = 0, w = 0;
	vec4() = default;
	vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
	vec4(const vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

inline vec3 operator*(const vec3& v, float s)
{
	return {v.x * s, v.y * s, v.z * s};
}

inline float max(float a, float b)
{
	return std::max(a, b);
}

inline float clamp(float v, float lo, float hi)
{
	return std::min(std::max(v, lo), hi);
}

inline vec3 clamp(const vec3& v, float lo, float hi)
{
	return {clamp(v.x, lo, hi), clamp(v.y, lo, hi), clamp(v.z, lo, hi)};
}

inline float mix(float a, float b, float t)
{
	return a * (1 - t) + b * t;
}

inline vec3 mix(const vec3& a, const vec3& b, float t)
{
	return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)};
}

#define SHADER_INLINE inline
#include "Render/Shaders/fog_share.glsl"
#include "Render/Shaders/particle_color.glsl"
#undef SHADER_INLINE

} // namespace dice::hfe::render::glsl
