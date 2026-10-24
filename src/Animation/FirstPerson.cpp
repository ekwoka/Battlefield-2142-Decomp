#include "FirstPerson.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::anim {

void FieldOfView::set(float to, float seconds)
{
	target = to;
	if (seconds <= 0) {
		current = to;
		rate = 0;
	} else {
		rate = std::fabs(current - to) / seconds;
	}
}

void FieldOfView::update(float seconds)
{
	if (current < target) {
		current = std::min(target, current + rate * seconds);
	} else if (current > target) {
		current = std::max(target, current - rate * seconds);
	}
}

float ZoomState::cycle(const ZoomSettings& settings)
{
	if (!settings.present()) {
		return 0;
	}
	m_level = (m_level + 1) % static_cast<int>(settings.factors.size());
	m_ticks = 0;
	return factor(settings);
}

void ZoomState::reset()
{
	m_level = 0;
	m_ticks = 0;
}

void ZoomState::tick()
{
	++m_ticks;
}

float ZoomState::factor(const ZoomSettings& settings) const
{
	return m_level > 0 && static_cast<size_t>(m_level) < settings.factors.size()
		? settings.factors[static_cast<size_t>(m_level)]
		: 0.0f;
}

int ZoomState::lod(const ZoomSettings& settings) const
{
	return m_level > 0 && m_ticks >= settings.delayTicks() ? settings.lod : 0;
}

void EyeHeight::tick(int pose)
{
	const float target = poses[pose >= 0 && pose < 3 ? pose : 0];
	current += (target - current) * kSpeed;
}

FirstPersonRig::FirstPersonRig(AnimationSystems& systems, const Skeleton& skeleton, const AnimationSystemTemplate* soldier)
	: m_systems(systems), m_skeleton(skeleton), m_soldierTemplate(soldier)
{
	if (soldier) {
		m_soldier = std::make_unique<AnimationState>(systems, *soldier);
	}
	m_pose = setupPose(skeleton);
	m_world = worldMatrices(skeleton, m_pose);
	m_cameraNode = skeleton.find("Camerabone");
	combineSprings();
}

void FirstPersonRig::setWeapon(const AnimationSystemTemplate* weapon)
{
	if (weapon == m_weaponTemplate) {
		return;
	}
	m_weaponTemplate = weapon;
	m_weapon = weapon ? std::make_unique<AnimationState>(m_systems, *weapon) : nullptr;
	// The new weapon's clips start from the setup pose.
	m_pose = setupPose(m_skeleton);
	combineSprings();
}

void FirstPersonRig::combineSprings()
{
	// 0x005ee980: the first-person system's spring, the held weapon's parts
	// over it (0x008bc390).
	m_hasSpring = false;
	m_spring = CameraSpring {};
	if (const CameraSpring* own = m_soldierTemplate ? m_soldierTemplate->cameraSpring() : nullptr) {
		m_spring = *own;
		m_hasSpring = true;
	}
	if (const CameraSpring* weapon = m_weaponTemplate ? m_weaponTemplate->cameraSpring() : nullptr) {
		if (m_hasSpring) {
			m_spring.overrideBy(*weapon);
		} else {
			m_spring = *weapon;
		}
		m_hasSpring = true;
	}
}

void FirstPersonRig::update(const AnimationInput& input, float seconds)
{
	if (m_soldier) {
		m_soldier->update(input, seconds);
		m_soldier->takeEvents();
	}
	if (m_weapon) {
		m_weapon->update(input, seconds);
		m_weapon->takeEvents(); // the first-person sounds and effects aren't played
	}
	std::vector<bool> touched;
	if (m_soldier) {
		m_soldier->apply(m_skeleton, m_pose, touched);
	}
	if (m_weapon) {
		m_weapon->apply(m_skeleton, m_pose, touched);
	}
	m_world = worldMatrices(m_skeleton, m_pose);
}

std::vector<std::string> FirstPersonRig::playing() const
{
	std::vector<std::string> names = m_soldier ? m_soldier->playing() : std::vector<std::string> {};
	if (m_weapon) {
		for (const std::string& name : m_weapon->playing()) {
			names.push_back(name);
		}
	}
	return names;
}

Matrix firstPersonPlacement(const Skeleton& skeleton, const std::vector<Matrix>& pose, int cameraNode,
	const Position& eye, float yaw, float pitch, const CameraSpring::V& springOffset)
{
	const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	// The view's axes as rows (row vectors: model x, y, z go to right, up,
	// forward).
	const float right[3] = {cy, 0, -sy};
	const float up[3] = {-sy * sp, cp, -cy * sp};
	const float forward[3] = {sy * cp, sp, cy * cp};
	float origin[3];
	for (int i = 0; i < 3; ++i) {
		origin[i] = eye[static_cast<size_t>(i)] + right[i] * springOffset.x + up[i] * springOffset.y
			+ forward[i] * springOffset.z;
	}
	const Matrix view = {right[0], right[1], right[2], 0, up[0], up[1], up[2], 0, forward[0], forward[1], forward[2], 0,
		origin[0], origin[1], origin[2], 1};
	(void)pose;
	if (cameraNode < 0 || static_cast<size_t>(cameraNode) >= skeleton.nodes.size()) {
		return view;
	}
	// The camera node's setup (bind) matrix, inverted (0x005ee860 keeps it
	// at soldier template +0x358, 0x006accd0); it is rigid: the inverse is
	// the transposed rotation and the translation taken back through it.
	Matrix c = matrixFromRotationTranslation(skeleton.nodes[static_cast<size_t>(cameraNode)].rotation,
		skeleton.nodes[static_cast<size_t>(cameraNode)].position);
	for (int parent = skeleton.nodes[static_cast<size_t>(cameraNode)].parent; parent >= 0;
		 parent = skeleton.nodes[static_cast<size_t>(parent)].parent) {
		c = multiply(c, matrixFromRotationTranslation(skeleton.nodes[static_cast<size_t>(parent)].rotation,
			skeleton.nodes[static_cast<size_t>(parent)].position));
	}
	Matrix inverse {};
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			inverse[static_cast<size_t>(i * 4 + j)] = c[static_cast<size_t>(j * 4 + i)];
		}
	}
	for (int j = 0; j < 3; ++j) {
		float sum = 0;
		for (int k = 0; k < 3; ++k) {
			sum += c[static_cast<size_t>(12 + k)] * inverse[static_cast<size_t>(k * 4 + j)];
		}
		inverse[static_cast<size_t>(12 + j)] = -sum;
	}
	inverse[15] = 1;
	return multiply(inverse, view);
}

} // namespace dice::hfe::anim
