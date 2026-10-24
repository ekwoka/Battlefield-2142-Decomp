#include "PlayerCamera.h"

#include "Game/GameWorld.h"
#include "Render/RenderMath.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dice::hfe {

namespace {

constexpr float kDegrees = 3.14159265f / 180.0f;

float number(const world::ObjectTemplate& t, const std::string& key, float otherwise)
{
	const auto it = t.settings().find(key);
	return it != t.settings().end() ? static_cast<float>(std::atof(it->second.c_str())) : otherwise;
}

std::string text(const world::ObjectTemplate& t, const std::string& key)
{
	const auto it = t.settings().find(key);
	return it != t.settings().end() ? it->second : std::string();
}

bool vec3(const std::string& value, float out[3])
{
	return std::sscanf(value.c_str(), "%f/%f/%f", &out[0], &out[1], &out[2]) == 3;
}

render::Mat4 placement(const world::Vec3& position, const world::Vec3& rotation)
{
	return render::Mat4::yawPitchRoll(rotation.x, rotation.y, rotation.z) *
		render::Mat4::translation({position.x, position.y, position.z});
}

// The view's yaw and pitch (radians) of a world matrix's forward row.
void anglesOf(const render::Mat4& m, float& yaw, float& pitch)
{
	const float x = m.at(2, 0), y = m.at(2, 1), z = m.at(2, 2);
	yaw = std::atan2(x, z);
	pitch = std::asin(std::clamp(y, -1.0f, 1.0f));
}

// RendDX9's setFov (0x10066ac0): for a = height / width of 0.8 or more
// (5:4 and squarer) the vertical field of view shrinks to keep 4:3's
// width (fov x a x 4/3); under 0.05625 (the binary's constant) x a x 16/9;
// in between (4:3 to 16:9) it is kept and the view widens.
float aspectFov(float fov, float aspect)
{
	const float a = aspect > 0 ? 1.0f / aspect : 0.75f;
	if (a >= 0.8f) {
		return fov * a * (4.0f / 3.0f);
	}
	if (a <= 0.05625f) {
		return fov * a * (16.0f / 9.0f);
	}
	return fov;
}

bool isCamera(const world::ObjectTemplate& t)
{
	return t.typeName() == "Camera";
}

} // namespace

PlayerCamera::PlayerCamera(game::GameWorld& round, anim::AnimationSystems& systems, Loader load)
	: m_round(round), m_systems(systems), m_load(std::move(load)), m_effects(round.templates())
{
}

PlayerCamera::~PlayerCamera() = default;

void PlayerCamera::resetSoldier()
{
	m_rig.reset();
	m_soldierTemplate = nullptr;
	m_weaponTemplate = nullptr;
	m_zoom.reset();
	m_roundsWas = -1;
	m_springState = {};
	m_springOffset = {};
}

void PlayerCamera::tick(float seconds)
{
	m_effects.update(seconds);
	const bool altPressed = m_altFire && !m_altFireWas;
	m_altFireWas = m_altFire;
	game::GameWorld::PlayerInfo me;
	const world::Object* soldier = m_round.player();
	const physics::SoldierBody* body = m_round.playerBody();
	if (!m_round.playerInfo(m_round.localPlayer(), me) || !me.alive || !soldier || !body) {
		if (m_alive) {
			resetSoldier();
		}
		m_alive = false;
		return;
	}
	const bool spawned = !m_alive;
	m_alive = true;
	// The soldier: its first-person skeleton and system, its camera's field
	// of view and start (the soldier's first child camera, 0x005890a0).
	const world::ObjectTemplate* soldierTemplate = soldier->objectTemplate();
	if (spawned || soldierTemplate != m_soldierTemplate) {
		resetSoldier();
		m_soldierTemplate = soldierTemplate;
		const std::string skeletonPath = text(*soldierTemplate, "skeleton1P");
		const anim::Skeleton* skeleton = skeletonPath.empty() ? nullptr : m_systems.skeleton(skeletonPath);
		const std::string systemPath = text(*soldierTemplate, "animationSystem1P");
		const anim::AnimationSystemTemplate* system = systemPath.empty() || !m_load ? nullptr : m_load(systemPath);
		if (skeleton) {
			m_rig = std::make_unique<anim::FirstPersonRig>(m_systems, *skeleton, system);
		}
		for (int pose = 0; pose < 3; ++pose) {
			float p[3];
			if (vec3(text(*soldierTemplate, "poseCameraPosition." + std::to_string(pose)), p)) {
				m_eye.poses[pose] = p[1]; // x and z are dropped (0x005c0c70)
			}
		}
		m_baseFov = 1.1f; // the camera template's default (0x00610fd0)
		m_cameraStart = 0;
		for (const world::ChildTemplate& child : soldierTemplate->children()) {
			const world::ObjectTemplate* t = m_round.templates().find(child.name);
			if (t && isCamera(*t)) {
				m_baseFov = number(*t, "insideFOV", 1.1f);
				m_cameraStart = child.position.y;
				break;
			}
		}
		m_eye.current = m_cameraStart;
		m_fov.set(m_baseFov, 0);
		m_lastYaw = me.yaw;
		m_lastPitch = me.aimPitch;
	}
	// The held weapon: its first-person system, zoom and muzzle flash.
	const world::Object* weapon = me.weapon ? m_round.objects().find(me.weapon) : nullptr;
	const world::ObjectTemplate* weaponTemplate = weapon ? weapon->objectTemplate() : nullptr;
	if (weaponTemplate != m_weaponTemplate) {
		m_weaponTemplate = weaponTemplate;
		const std::string systemPath = weaponTemplate ? text(*weaponTemplate, "animationSystem1P") : std::string();
		if (m_rig) {
			m_rig->setWeapon(systemPath.empty() || !m_load ? nullptr : m_load(systemPath));
		}
		m_zoomSettings = {};
		m_muzzle = m_zoomMuzzle = {};
		if (weaponTemplate) {
			const auto& components = weaponTemplate->components();
			if (std::find(components.begin(), components.end(), "DefaultZoomComp") != components.end()) {
				std::istringstream factors(text(*weaponTemplate, "zoom.addZoomFactor.list"));
				for (float f; factors >> f;) {
					if (std::find(m_zoomSettings.factors.begin(), m_zoomSettings.factors.end(), f)
						== m_zoomSettings.factors.end()) {
						m_zoomSettings.factors.push_back(f); // duplicates skipped (0x005dae50)
					}
				}
				m_zoomSettings.delay = number(*weaponTemplate, "zoom.zoomDelay", 0.5f);
				m_zoomSettings.lod = static_cast<int>(number(*weaponTemplate, "zoom.zoomLod", 0));
				m_zoomSettings.disableMuzzle = number(*weaponTemplate, "zoom.disableMuzzleWhenZoomed", 0) != 0;
				m_zoomSettings.mouseSpeedMod = number(*weaponTemplate, "zoom.mouseSpeedMod", 1);
				m_zoomSettings.zoomOutOnReload = number(*weaponTemplate, "zoom.zoomOutOnReload", 1) != 0;
			}
			for (const world::ChildTemplate& child : weaponTemplate->children()) {
				if (child.name.compare(0, 6, "e_muzz") != 0) {
					continue;
				}
				const bool zoom = child.name.find("zoom") != std::string::npos || child.name.find("Zoom") != std::string::npos;
				Muzzle& muzzle = zoom ? m_zoomMuzzle : m_muzzle;
				if (muzzle.effect.empty()) {
					muzzle = {child.name, {child.position.x, child.position.y, child.position.z}};
				}
			}
		}
		m_zoom.reset();
		m_fov.set(m_baseFov, 0);
		m_roundsWas = -1;
	}
	const game::FireArm* firearm = m_round.playerWeapon();
	const bool reloading = firearm && firearm->reloading();
	const int rounds = firearm ? firearm->roundsInMag() : -1;
	const bool fired = m_roundsWas >= 0 && rounds >= 0 && rounds < m_roundsWas;
	m_roundsWas = rounds;
	// The zoom: the alternate fire's press steps it (0x005da670), a reload
	// zooms out (0x005da630); the player's field of view follows over
	// zoomDelay (0x005da3f0 -> 0x00591740).
	if (!me.vehicle && m_zoomSettings.present()) {
		if (altPressed && !reloading) {
			const float factor = m_zoom.cycle(m_zoomSettings);
			m_fov.set(factor > 0 ? factor : m_baseFov, m_zoomSettings.delay);
		} else if (reloading && !m_reloadingWas && m_zoom.level() > 0 && m_zoomSettings.zoomOutOnReload) {
			m_zoom.reset();
			m_fov.set(m_baseFov, m_zoomSettings.delay);
		}
	}
	m_reloadingWas = reloading;
	if (me.vehicle && m_zoom.level() > 0) {
		m_zoom.reset();
		m_fov.set(m_baseFov, 0);
	}
	m_zoom.tick();
	m_fov.update(seconds);
	// The eye's height by pose (seated counts as crouching).
	const int pose = me.vehicle ? 1 : body->stance >= 0 && body->stance < 3 ? body->stance : 0;
	m_eye.tick(pose);
	if (!m_rig || me.vehicle) {
		m_lastYaw = me.yaw;
		m_lastPitch = me.aimPitch;
		return;
	}
	// The first-person systems see what the third-person ones do (pose,
	// movement in the soldier's frame, fire and reload), and the zoom
	// (message 4).
	anim::AnimationInput input;
	const physics::Vec3& v = body->velocity;
	const float speed = std::sqrt(v.x * v.x + v.z * v.z);
	const float yaw = body->yawDegrees * kDegrees;
	input.speed = speed;
	if (speed > 0.01f) {
		input.forward = (v.x * std::sin(yaw) + v.z * std::cos(yaw)) / speed;
		input.side = (v.x * std::cos(yaw) - v.z * std::sin(yaw)) / speed;
	}
	input.movementSeconds = seconds;
	input.pose = body->stance;
	if (reloading) {
		input.messages |= 2;
	}
	if (m_zoom.level() > 0) {
		input.messages |= 4;
	}
	if (fired) {
		input.pulses |= 1;
	}
	m_rig->update(input, seconds);
	// The camera spring (0x005ee980): the look's turn and pitch this tick,
	// the velocity along the look's right, up and forward, a shot's kick.
	if (const anim::CameraSpring* spring = m_rig->spring()) {
		anim::CameraSpringState::Input springInput;
		springInput.yawDelta = std::remainder(me.yaw - m_lastYaw, 360.0f);
		springInput.pitchDelta = me.aimPitch - m_lastPitch;
		const float look = me.yaw * kDegrees;
		springInput.right = v.x * std::cos(look) - v.z * std::sin(look);
		springInput.up = v.y;
		springInput.forward = v.x * std::sin(look) + v.z * std::cos(look);
		springInput.recoil = fired ? 1.0f : 0.0f;
		const float zoomedFov = m_zoom.factor(m_zoomSettings);
		springInput.zoom = zoomedFov > 0 && zoomedFov < m_baseFov
			? std::clamp((m_baseFov - m_fov.current) / (m_baseFov - zoomedFov), 0.0f, 1.0f)
			: 0.0f;
		m_springOffset = m_springState.update(*spring, springInput, seconds);
	}
	m_lastYaw = me.yaw;
	m_lastPitch = me.aimPitch;
	// The muzzle flash: the weapon's child at its place on the animated
	// first-person weapon (its root is the skeleton's mesh1), the zoomed one
	// while zoomed in with disableMuzzleWhenZoomed.
	const Muzzle& muzzle = m_zoomSettings.disableMuzzle && zoomed() ? m_zoomMuzzle : m_muzzle;
	if (fired && firstPerson && !muzzle.effect.empty()) {
		View eye;
		if (view(eye, 4.0f / 3.0f)) {
			const anim::Skeleton& skeleton = m_rig->skeleton();
			const int mesh1 = skeleton.find("mesh1");
			const anim::Matrix place = anim::firstPersonPlacement(skeleton, m_rig->pose(), m_rig->cameraNode(),
				eye.position, eye.yaw, eye.pitch, m_springOffset);
			const anim::Matrix part = mesh1 >= 0 ? anim::multiply(m_rig->pose()[static_cast<size_t>(mesh1)], place) : place;
			const float* o = muzzle.offset.data();
			const physics::Vec3 at {o[0] * part[0] + o[1] * part[4] + o[2] * part[8] + part[12],
				o[0] * part[1] + o[1] * part[5] + o[2] * part[9] + part[13],
				o[0] * part[2] + o[1] * part[6] + o[2] * part[10] + part[14]};
			physics::Mat3 rotation;
			for (int r = 0; r < 3; ++r) {
				rotation.rows[static_cast<size_t>(r)] = {part[static_cast<size_t>(r * 4)],
					part[static_cast<size_t>(r * 4 + 1)], part[static_cast<size_t>(r * 4 + 2)]};
			}
			m_effects.spawn(muzzle.effect, at, rotation);
		}
	}
}

bool PlayerCamera::view(View& view, float aspect) const
{
	if (!firstPerson || !m_alive) {
		return false;
	}
	game::GameWorld::PlayerInfo me;
	const physics::SoldierBody* body = m_round.playerBody();
	if (!m_round.playerInfo(m_round.localPlayer(), me) || !me.alive || !body) {
		return false;
	}
	if (me.vehicle) {
		if (!seatCamera(view)) {
			return false;
		}
		view.fovY = aspectFov(view.fovY, aspect);
		return true;
	}
	// The soldier camera: its local position is (0, eye, 0) in the body's
	// frame, turned by the look (the body's yaw plus the head's twist) and
	// the aim's pitch (0x005890a0: Rx(pitch) x Ry(yaw), slot 0xe8).
	view.position = {body->position.x, body->position.y + m_eye.current, body->position.z};
	view.yaw = me.yaw * kDegrees;
	view.pitch = std::clamp(me.aimPitch, -85.0f, 85.0f) * kDegrees;
	view.fovY = aspectFov(m_fov.current, aspect);
	return true;
}

// A seat's camera (Player::getCamera 0x005948e0 -> the seat's 0x005c7cb0):
// the first Camera in the seat's subtree (its nested seats left out),
// placed by the template tree (the guns' bundles turned as the game turns
// them), its insideFOV.
bool PlayerCamera::seatCamera(View& view) const
{
	game::GameWorld::PlayerInfo me;
	m_round.playerInfo(m_round.localPlayer(), me);
	const world::Object* vehicle = m_round.objects().find(me.vehicle);
	if (!vehicle || !vehicle->objectTemplate()) {
		return false;
	}
	const auto seats = m_round.vehicleSeats(vehicle->id());
	if (me.seat < 0 || static_cast<size_t>(me.seat) >= seats.size()) {
		return false;
	}
	const std::string& seatName = seats[static_cast<size_t>(me.seat)].name;
	const world::ObjectTemplateManager& templates = m_round.templates();
	const uint32_t object = vehicle->id();
	// Depth-first through the tree: find the seat, then its camera.
	struct Found {
		bool ok = false;
		render::Mat4 local;
		const world::ObjectTemplate* camera = nullptr;
	} found;
	std::function<void(const world::ObjectTemplate&, const render::Mat4&, bool, int)> walk =
		[&](const world::ObjectTemplate& t, const render::Mat4& local, bool inSeat, int depth) {
			if (found.ok || depth > 10) {
				return;
			}
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* c = templates.find(child.name);
				if (!c) {
					continue;
				}
				render::Mat4 m = placement(child.position, child.rotation) * local;
				world::Vec3 offset, rotation;
				if (m_round.partPose(object, child.name, offset, rotation)) {
					m = placement(offset, rotation) * m;
				}
				const bool seat = c->settings().count("seatInformation") != 0;
				if (inSeat && seat) {
					continue; // another seat's cameras
				}
				if (inSeat && isCamera(*c)) {
					found = {true, m, c};
					return;
				}
				walk(*c, m, inSeat || (seat && c->name() == seatName), depth + 1);
				if (found.ok) {
					return;
				}
			}
		};
	const world::ObjectTemplate& root = *vehicle->objectTemplate();
	walk(root, render::Mat4 {}, root.name() == seatName, 0);
	if (!found.ok) {
		return false;
	}
	const render::Mat4 world = found.local * placement(vehicle->position(), vehicle->rotation());
	view.position = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
	anglesOf(world, view.yaw, view.pitch);
	view.fovY = number(*found.camera, "insideFOV", 1.1f);
	return true;
}

std::vector<PlayerCamera::Model> PlayerCamera::models() const
{
	std::vector<Model> out;
	View eye;
	game::GameWorld::PlayerInfo me;
	if (!m_rig || !view(eye, 4.0f / 3.0f) || !m_round.playerInfo(m_round.localPlayer(), me) || me.vehicle) {
		return out;
	}
	const anim::Skeleton& skeleton = m_rig->skeleton();
	const std::vector<anim::Matrix>& pose = m_rig->pose();
	const anim::Matrix place =
		anim::firstPersonPlacement(skeleton, pose, m_rig->cameraNode(), eye.position, eye.yaw, eye.pitch, m_springOffset);
	// The arms: the soldier mesh's first-person geometry.
	if (m_soldierTemplate && !m_soldierTemplate->geometry().empty()) {
		Model arms;
		arms.geometry = m_soldierTemplate->geometry();
		arms.world = place;
		arms.pose = pose;
		out.push_back(std::move(arms));
	}
	// The weapon's first-person geometry, its parts on mesh1, mesh2...
	if (m_weaponTemplate && !m_weaponTemplate->geometry().empty()) {
		Model weapon;
		weapon.geometry = m_weaponTemplate->geometry();
		weapon.lod = m_zoom.lod(m_zoomSettings);
		weapon.world = place;
		const anim::Matrix identity {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		for (int part = 1; part <= 16; ++part) {
			const int node = skeleton.find("mesh" + std::to_string(part));
			weapon.parts.push_back(node >= 0 ? pose[static_cast<size_t>(node)] : identity);
		}
		out.push_back(std::move(weapon));
	}
	return out;
}

bool PlayerCamera::hides(uint32_t object) const
{
	if (!firstPerson || !m_alive || object == 0) {
		return false;
	}
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(m_round.localPlayer(), me) || !me.alive) {
		return false;
	}
	return object == me.soldier || object == me.weapon || object == me.kit;
}

bool PlayerCamera::zoomed() const
{
	return m_zoom.level() > 0 && m_fov.current == m_fov.target;
}

float PlayerCamera::mouseScale() const
{
	if (m_zoom.level() <= 0 || m_baseFov <= 0) {
		return 1;
	}
	return m_fov.current / m_baseFov * m_zoomSettings.mouseSpeedMod;
}

std::vector<std::string> PlayerCamera::playing() const
{
	return m_rig ? m_rig->playing() : std::vector<std::string> {};
}

} // namespace dice::hfe
