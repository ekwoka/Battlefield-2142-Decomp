#include "PlayerCamera.h"

#include "Game/GameWorld.h"
#include "Render/RenderMath.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

game::camera::Frame frameOf(const render::Mat4& m)
{
	game::camera::Frame f;
	for (int r = 0; r < 3; ++r) {
		f.rotation.rows[static_cast<size_t>(r)] = {m.at(r, 0), m.at(r, 1), m.at(r, 2)};
	}
	f.position = {m.at(3, 0), m.at(3, 1), m.at(3, 2)};
	return f;
}

game::camera::Vec3 vec3Of(const std::string& value, game::camera::Vec3 fallback)
{
	float v[3];
	return vec3(value, v) ? game::camera::Vec3 {v[0], v[1], v[2]} : fallback;
}

// An object's bounding radius (slot 0x90, the hierarchy's): the rewrite
// takes a vehicle's box (its collision LOD's farthest corner) and 1 m for
// anything else (a soldier).
float radiusOf(game::GameWorld& round, uint32_t object)
{
	for (const auto& v : round.vehicleList()) {
		if (v.object == object) {
			const auto far = [](const physics::Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); };
			return std::max(far(v.boxMin), far(v.boxMax));
		}
	}
	return 1.0f;
}

} // namespace

PlayerCamera::PlayerCamera(game::GameWorld& round, anim::AnimationSystems& systems, Loader load)
	: m_round(round), m_systems(systems), m_load(std::move(load)), m_effects(round.templates())
{
}

PlayerCamera::~PlayerCamera() = default;

bool PlayerCamera::current(Me& me) const
{
	if (m_remote) {
		Remote remote;
		if (!m_remote(remote)) {
			return false;
		}
		me.alive = remote.alive;
		me.soldier = remote.soldier;
		me.weapon = remote.weapon;
		me.kit = remote.kit;
		me.soldierTemplate = remote.soldierTemplate;
		me.weaponTemplate = remote.weaponTemplate;
		me.origin = {remote.feet[0], remote.feet[1] + 1.0f, remote.feet[2]};
		me.velocity = remote.velocity;
		me.bodyYaw = me.yaw = remote.yaw;
		me.aimPitch = remote.aimPitch;
		me.stance = remote.stance;
		me.firing = remote.firing;
		return true;
	}
	game::GameWorld::PlayerInfo info;
	const world::Object* soldier = m_round.player();
	const physics::SoldierBody* body = m_round.playerBody();
	if (!m_round.playerInfo(m_round.localPlayer(), info) || !soldier || !body) {
		return false;
	}
	me.alive = info.alive;
	me.soldier = info.soldier;
	me.weapon = info.weapon;
	me.kit = info.kit;
	me.vehicle = info.vehicle;
	me.seat = info.seat;
	me.soldierTemplate = soldier->objectTemplate();
	const world::Object* weapon = info.weapon ? m_round.objects().find(info.weapon) : nullptr;
	me.weaponTemplate = weapon ? weapon->objectTemplate() : nullptr;
	me.origin = {body->position.x, body->position.y, body->position.z};
	me.velocity = {body->velocity.x, body->velocity.y, body->velocity.z};
	me.bodyYaw = body->yawDegrees;
	me.yaw = info.yaw;
	me.aimPitch = info.aimPitch;
	me.stance = body->stance;
	if (const game::FireArm* firearm = m_round.playerWeapon()) {
		me.reloading = firearm->reloading();
		me.rounds = firearm->roundsInMag();
	}
	return true;
}

void PlayerCamera::resetSoldier()
{
	m_effects.releaseAnchor(kWeaponAnchor);
	m_rig.reset();
	m_soldierTemplate = nullptr;
	m_weaponTemplate = nullptr;
	m_zoom.reset();
	m_roundsWas = -1;
	m_springState = {};
	m_springOffset = {};
}

struct PlayerCamera::SeatView {
	uint32_t vehicle = 0;
	const world::ObjectTemplate* seat = nullptr;
	const world::ObjectTemplate* camera = nullptr;
	render::Mat4 world;
	// The camera's frames up its tree, in the world: [0] the camera, [1]
	// its parent ... the last the vehicle (chaseParentIndex counts these).
	std::vector<render::Mat4> chain;
};

void PlayerCamera::stepShakes(float seconds)
{
	m_clock += seconds;
	SeatView seat;
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(m_round.localPlayer(), me) || !me.alive || !me.vehicle || !findSeatCamera(seat)) {
		m_speedShake = m_hitShake = m_shakeTime = m_hitLeft = 0;
		m_lastVehicleHp = -1;
		m_shakeVehicle = 0;
		return;
	}
	const world::ObjectTemplate& camera = *seat.camera;
	// The speed shake.
	const float start = number(camera, "cameraShakeStartSpeed", 0), most = number(camera, "cameraShakeMaxSpeed", 0);
	float speed = 0;
	for (const auto& v : m_round.vehicleList()) {
		if (v.object == me.vehicle) {
			speed = std::sqrt(v.velocity.x * v.velocity.x + v.velocity.y * v.velocity.y + v.velocity.z * v.velocity.z);
		}
	}
	if (start > 0 && speed > start) {
		m_shakeTime += seconds;
	}
	m_speedShake = anim::cameraSpeedShake(m_shakeTime, speed, start, most);
	// The hit shake: a hit's share of the vehicle's hit points.
	const float hp = m_round.hitPoints(me.vehicle);
	const world::Object* vehicle = m_round.objects().find(me.vehicle);
	const float maxHp = vehicle && vehicle->objectTemplate() ? number(*vehicle->objectTemplate(), "armor.maxHitPoints", 0) : 0;
	if (m_shakeVehicle == me.vehicle && m_lastVehicleHp >= 0 && hp < m_lastVehicleHp && maxHp > 0) {
		m_hitLeft = anim::kCameraHitShakeTime;
		m_hitStrength = (m_lastVehicleHp - hp) / maxHp;
	}
	m_shakeVehicle = me.vehicle;
	m_lastVehicleHp = hp;
	m_hitShake = 0;
	if (m_hitLeft > 0) {
		m_hitLeft = std::max(0.0f, m_hitLeft - seconds);
		m_hitShake = anim::cameraHitShake(m_hitLeft, m_hitStrength);
	}
}

float PlayerCamera::seatFov(bool inside) const
{
	SeatView seat;
	if (!findSeatCamera(seat)) {
		return 0;
	}
	return number(*seat.camera, inside ? "insideFOV" : "worldFOV", 1.1f);
}

bool PlayerCamera::insideSun(InsideSun& sun) const
{
	SeatView seat;
	if (!insideView() || !findSeatCamera(seat)) {
		return false;
	}
	const world::ObjectTemplate& c = *seat.camera;
	if (number(c, "allowInsideStaticSun", 0) == 0) {
		return false;
	}
	sun.dynamicLights = number(c, "allowInsideDynamicLights", 1) != 0;
	const auto vec = [&c](const char* name, std::array<float, 3> fallback) {
		const auto it = c.settings().find(name);
		if (it != c.settings().end()) {
			std::sscanf(it->second.c_str(), "%f/%f/%f", &fallback[0], &fallback[1], &fallback[2]);
		}
		return fallback;
	};
	sun.direction = vec("insideStaticSunDirection", sun.direction);
	sun.color = vec("insideStaticSunColor", sun.color);
	sun.ambient = vec("insideStaticSunAmbientColor", sun.ambient);
	const world::Object* vehicle = m_round.objects().find(seat.vehicle);
	const float maxHp = vehicle && vehicle->objectTemplate() ? number(*vehicle->objectTemplate(), "armor.maxHitPoints", 0) : 0;
	const float share = maxHp > 0 ? m_round.hitPoints(seat.vehicle) / maxHp : 1;
	if (share < number(c, "insideDamageThreshold", 0.15f)) {
		const float frequency = number(c, "insideDamageFrequency", 3);
		if (std::fmod(m_clock * frequency, 1.0f) < 0.5f) {
			sun.color = vec("insideStaticSunDamageColor", {1, 0, 0});
		}
	}
	return true;
}

game::camera::Ground PlayerCamera::ground() const
{
	game::camera::Ground g;
	g.terrain = [this](float x, float z) { return m_round.terrainHeight(x, z); };
	float sea = 0;
	if (m_round.waterLevel(sea)) {
		g.sea = [sea](float, float) { return sea; };
	}
	return g;
}

game::camera::ModeRules PlayerCamera::modeRules() const
{
	game::camera::ModeRules rules;
	rules.allowNoseCam = allowNoseCam;
	rules.allowExternalViews = allowExternalViews;
	SeatView seat;
	if (findSeatCamera(seat)) {
		const world::ObjectTemplate& c = *seat.camera;
		rules.inside = number(c, "CVMInside", 1) != 0;
		rules.nose = number(c, "CVMNose", 0) != 0;
		rules.chase = number(c, "CVMChase", 1) != 0;
		rules.frontChase = number(c, "CVMFrontChase", 1) != 0;
		rules.flyBy = number(c, "CVMFlyBy", 1) != 0;
		rules.trace = number(c, "CVMTrace", 0) != 0;
		rules.externTrace = number(c, "CVMExternTrace", 0) != 0;
	}
	return rules;
}

bool PlayerCamera::setMode(game::camera::Mode mode)
{
	using game::camera::Mode;
	if (mode == Mode::FirstPerson) {
		mode = Mode::Inside;
	}
	if (!game::camera::modeAllowed(mode, modeRules())) {
		return false;
	}
	// 0x00612eb0: Chase and FrontChase start over (0x006105c0), FlyBy drops
	// its points, Trace puts its point 20 m ahead.
	m_mode = mode;
	m_chase.reset();
	m_flyBy.reset();
	m_traceStarted = false;
	return true;
}

void PlayerCamera::toggleMode(bool backwards)
{
	game::GameWorld::PlayerInfo me;
	const bool alive = !m_remote && m_round.playerInfo(m_round.localPlayer(), me) && me.alive;
	if (!m_remote && !alive && m_spectating == Spectating::Follow) {
		nextPlayer();
		return;
	}
	if (!m_remote && !alive && m_spectating == Spectating::Death && allowFreeCam) {
		// 0x00615e00: the key picks a teammate to follow.
		nextPlayer();
		if (m_followPlayer >= 0) {
			m_spectating = Spectating::Follow;
			m_follow = {};
		}
		return;
	}
	if (alive && me.vehicle) {
		const game::camera::Mode next = game::camera::nextMode(m_mode, modeRules(), backwards);
		setMode(next);
		firstPerson = next == game::camera::Mode::Inside;
		return;
	}
	firstPerson = !firstPerson || !allowExternalViews;
}

bool PlayerCamera::insideView() const
{
	if (!m_alive) {
		return false;
	}
	game::GameWorld::PlayerInfo me;
	if (!m_remote && m_round.playerInfo(m_round.localPlayer(), me) && me.vehicle) {
		return m_mode == game::camera::Mode::Inside;
	}
	return firstPerson;
}

void PlayerCamera::nextPlayer()
{
	// 0x00615ce0: the living teammates (not spectators) in the players'
	// order, the one after the followed one, else the first.
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(m_round.localPlayer(), me)) {
		return;
	}
	int first = -1;
	bool passed = false;
	for (const auto& p : m_round.playerList()) {
		if (p.team != me.team || !p.alive || p.id == me.id) {
			continue;
		}
		if (p.id == m_followPlayer) {
			passed = true;
			continue;
		}
		if (first == -1) {
			first = p.id;
		}
		if (passed) {
			m_followPlayer = p.id;
			return;
		}
	}
	if (first != -1) {
		m_followPlayer = first;
	}
}

void PlayerCamera::spectatorAction()
{
	// 0x00616110: only for the dead (the camera not attached), with
	// sv.allowFreeCam; it swaps flying free (mode 1) and mode 6.
	game::GameWorld::PlayerInfo me;
	if (m_remote || !allowFreeCam || !m_round.playerInfo(m_round.localPlayer(), me) || me.alive
		|| m_spectating == Spectating::None) {
		return;
	}
	if (m_spectating != Spectating::Free) {
		// From the view now (the matrix +0x2a0 handed over).
		m_free.position = m_external.eye;
		float yaw = 0, pitch = 0;
		game::camera::anglesOf(m_external, yaw, pitch);
		m_free.yaw = yaw / kDegrees;
		m_free.pitch = pitch / kDegrees;
		m_spectating = Spectating::Free;
		return;
	}
	// setMode 6 with no object (0x00615f10): the kept one (+0x198) again,
	// else the next teammate; none, and it stays free.
	if (m_deathObject && m_round.objects().find(m_deathObject)) {
		const world::Object* o = m_round.objects().find(m_deathObject);
		game::camera::deathStart(m_death, {o->position().x, o->position().y, o->position().z},
			game::camera::normalized(m_external.at - m_external.eye), radiusOf(m_round, m_deathObject));
		m_spectating = Spectating::Death;
		return;
	}
	nextPlayer();
	if (m_followPlayer >= 0) {
		m_spectating = Spectating::Follow;
		m_follow = {};
	}
}

void PlayerCamera::stepViews(float seconds)
{
	using namespace game::camera;
	m_hasExternal = false;
	if (m_remote) {
		return; // a joined game's views stay the first person and the chase
	}
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(m_round.localPlayer(), me)) {
		return;
	}
	const Ground g = ground();
	if (me.alive) {
		m_spectating = Spectating::None;
		m_followPlayer = -1;
		m_deathObject = 0;
		m_lastControlObject = me.vehicle ? me.vehicle : me.soldier;
		SeatView seat;
		if (!me.vehicle || !findSeatCamera(seat)) {
			m_modeVehicle = 0;
			m_modeSeat = -1;
			m_mode = Mode::Inside;
			const float yaw = me.yaw * kDegrees, pitch = me.aimPitch * kDegrees;
			m_lastLook.eye = {me.position.x, me.position.y + 1.0f + m_eye.current, me.position.z};
			m_lastLook.at = m_lastLook.eye
				+ Vec3 {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
			return;
		}
		if (me.vehicle != m_modeVehicle || me.seat != m_modeSeat) {
			// A new seat: Inside (its camera is set up so on entering), or
			// Chase for a third-person player.
			m_modeVehicle = me.vehicle;
			m_modeSeat = me.seat;
			m_mode = Mode::Inside;
			if (!firstPerson ? !setMode(Mode::Chase) : !setMode(Mode::Inside)) {
				setMode(nextMode(Mode::Inside, modeRules()));
			}
		} else if (!modeAllowed(m_mode, modeRules())) {
			setMode(Mode::Inside);
		}
		m_flyBy.clock += seconds;
		const Frame camera = frameOf(seat.world);
		const Frame parent = seat.chain.size() > 1 ? frameOf(seat.chain[1]) : camera;
		const Frame root = frameOf(seat.chain.back());
		const world::ObjectTemplate& c = *seat.camera;
		Vec3 velocity;
		for (const auto& v : m_round.vehicleList()) {
			if (v.object == me.vehicle) {
				velocity = v.velocity;
			}
		}
		const float radius = radiusOf(m_round, me.vehicle);
		Look look;
		switch (m_mode) {
		case Mode::Nose: look = nose(camera, parent.rotation, vec3Of(text(c, "nosePos"), {})); break;
		case Mode::Chase:
		case Mode::FrontChase: {
			ChaseSettings settings;
			settings.distance = number(c, "chaseDistance", -1);
			settings.angle = number(c, "chaseAngle", 0.3f);
			settings.offset = vec3Of(text(c, "chaseOffset"), {});
			settings.stiffness = number(c, "followStiffness", 1);
			const int index = static_cast<int>(number(c, "chaseParentIndex", -1));
			const Frame chaseParent = index == -1 ? root
				: frameOf(seat.chain[static_cast<size_t>(std::min<int>(index, static_cast<int>(seat.chain.size()) - 1))]);
			look = chase(m_chase, settings, chaseParent, root.position, radius, m_mode == Mode::FrontChase, seconds, g);
			break;
		}
		case Mode::FlyBy: look = flyBy(m_flyBy, camera.position, root, velocity, radius, g); break;
		case Mode::Trace: {
			if (!m_traceStarted) {
				traceStart(m_trace, camera.position, parent);
				m_traceStarted = true;
			}
			// The player's slot 0x338 player (its killer) while it lives.
			game::GameWorld::PlayerInfo other;
			Vec3 at;
			const bool target = me.killer >= 0 && m_round.playerInfo(me.killer, other) && other.alive;
			if (target) {
				at = other.position;
			}
			look = trace(m_trace, camera.position, parent, target ? &at : nullptr);
			break;
		}
		default:
			look.eye = camera.position;
			look.at = camera.position + camera.forward();
			break;
		}
		m_lastLook = look;
		if (m_mode != Mode::Inside) {
			m_external = look;
			m_externalFov = number(c, "worldFOV", 1.1f); // 0x00610150
			m_hasExternal = true;
		}
		return;
	}
	// Dead: the death camera starts on the object the player was in (the
	// death 0x004cd390: setMode 6 with the player's control object).
	if (m_spectating == Spectating::None) {
		if (!m_alive || !m_lastControlObject) {
			return; // never spawned yet: the before-spawn camera
		}
		m_deathObject = m_lastControlObject;
		const world::Object* o = m_round.objects().find(m_deathObject);
		const Vec3 at = o ? Vec3 {o->position().x, o->position().y, o->position().z} : m_lastLook.eye;
		deathStart(m_death, at, normalized(m_lastLook.at - m_lastLook.eye), radiusOf(m_round, m_deathObject));
		m_spectating = Spectating::Death;
	}
	Look look;
	if (m_spectating == Spectating::Follow) {
		game::GameWorld::PlayerInfo other;
		if (!m_round.playerInfo(m_followPlayer, other) || !other.alive) {
			const int was = m_followPlayer;
			m_followPlayer = -1;
			nextPlayer();
			if (m_followPlayer < 0 || m_followPlayer == was) {
				m_followPlayer = -1;
				m_spectating = Spectating::Death;
			}
		}
	}
	if (m_spectating == Spectating::Follow) {
		game::GameWorld::PlayerInfo other;
		m_round.playerInfo(m_followPlayer, other);
		Frame frame;
		const uint32_t object = other.vehicle ? other.vehicle : other.soldier;
		const world::Object* o = m_round.objects().find(object);
		if (other.vehicle && o) {
			frame.rotation = Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z});
			frame.position = {o->position().x, o->position().y, o->position().z};
		} else {
			// Its soldier camera: at the eye, turned by the look.
			frame.rotation = Mat3::fromYawPitchRoll({other.yaw, -other.aimPitch, 0});
			frame.position = {other.position.x, other.position.y + 1.47f, other.position.z};
		}
		const Vec3 velocity = other.velocity;
		look = follow(m_follow, frame, radiusOf(m_round, object), &velocity, g);
	} else if (m_spectating == Spectating::Free) {
		freeMove(m_free, m_freeInput, seconds, g);
		m_freeInput.turnDegrees = m_freeInput.pitchDegrees = 0;
		look = freeLook(m_free);
	} else {
		const world::Object* o = m_deathObject ? m_round.objects().find(m_deathObject) : nullptr;
		Vec3 body, killer;
		if (o) {
			body = {o->position().x, o->position().y, o->position().z};
		}
		game::GameWorld::PlayerInfo other;
		const bool hasKiller = me.killer >= 0 && m_round.playerInfo(me.killer, other) && other.alive;
		if (hasKiller) {
			const world::Object* k = m_round.objects().find(other.vehicle ? other.vehicle : other.soldier);
			killer = k ? Vec3 {k->position().x, k->position().y, k->position().z} : other.position;
		}
		look = death(m_death, o ? &body : nullptr, hasKiller ? &killer : nullptr, seconds, g);
	}
	m_external = look;
	m_externalFov = m_baseFov;
	m_hasExternal = true;
}

bool PlayerCamera::externalView(View& view, float aspect) const
{
	if (!m_hasExternal) {
		return false;
	}
	view.position = {m_external.eye.x, m_external.eye.y, m_external.eye.z};
	game::camera::anglesOf(m_external, view.yaw, view.pitch);
	view.fovY = aspectFov(m_externalFov, aspect);
	view.roll = 0;
	return true;
}

void PlayerCamera::tick(float seconds)
{
	m_effects.update(seconds);
	stepShakes(seconds);
	stepViews(seconds);
	++m_tick; // the game's tick counter (0x00666090)
	m_recoilNow = 0;
	const bool altPressed = m_altFire && !m_altFireWas;
	m_altFireWas = m_altFire;
	Me me;
	if (!current(me) || !me.alive || !me.soldierTemplate) {
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
	const world::ObjectTemplate* soldierTemplate = me.soldierTemplate;
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
	const world::ObjectTemplate* weaponTemplate = me.weaponTemplate;
	if (weaponTemplate != m_weaponTemplate) {
		m_weaponTemplate = weaponTemplate;
		const std::string systemPath = weaponTemplate ? text(*weaponTemplate, "animationSystem1P") : std::string();
		if (m_rig) {
			m_rig->setWeapon(systemPath.empty() || !m_load ? nullptr : m_load(systemPath));
		}
		m_zoomSettings = {};
		m_muzzle = m_zoomMuzzle = {};
		m_weaponBundles.clear();
		m_recoil = {};
		m_recoilGraph.clear();
		m_lastShotTick = -1000;
		if (weaponTemplate) {
			const auto& has = weaponTemplate->components();
			if (std::find(has.begin(), has.end(), "SoldierBasedRecoilComp") != has.end()) {
				m_recoil.hasRecoilForce = number(*weaponTemplate, "recoil.hasRecoilForce", 0) != 0;
				m_recoil.goBackOnRecoil = number(*weaponTemplate, "recoil.goBackOnRecoil", 1) != 0;
				m_recoil.zoomModifier = number(*weaponTemplate, "recoil.zoomModifier", 1);
				m_recoil.graphFrameCount = static_cast<int>(number(*weaponTemplate, "recoil.recoilGraphFrameCount", 8));
				m_recoil.graphExponent = number(*weaponTemplate, "recoil.recoilGraphExponent", 2);
				m_recoil.graphTotalMovement = number(*weaponTemplate, "recoil.recoilGraphTotalMovement", 2);
				m_recoilGraph = m_recoil.graph();
				m_recoilForceUp = game::RandomDistribution::parse(text(*weaponTemplate, "recoil.recoilForceUp"));
			}
			// The effect bundles its events start, anywhere in its tree.
			const std::function<void(const world::ObjectTemplate&, std::array<float, 3>, int)> bundles =
				[&](const world::ObjectTemplate& t, std::array<float, 3> at, int depth) {
					if (depth > 8) {
						return;
					}
					for (const world::ChildTemplate& child : t.children()) {
						const world::ObjectTemplate* c = m_round.templates().find(child.name);
						if (!c) {
							continue;
						}
						const std::array<float, 3> place {at[0] + child.position.x, at[1] + child.position.y,
							at[2] + child.position.z};
						if (c->typeName() == "EffectBundle") {
							std::string start = text(*c, "setStartOnEffects");
							if (start.empty()) {
								start = text(*c, "startOnEffects");
							}
							const uint32_t bits = static_cast<uint32_t>(std::strtoul(start.c_str(), nullptr, 10));
							if (bits != 0) {
								m_weaponBundles.push_back({c->name(), place, bits});
							}
							continue;
						}
						bundles(*c, place, depth + 1);
					}
				};
			bundles(*weaponTemplate, {0, 0, 0}, 0);
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
	const bool reloading = me.reloading;
	const int rounds = me.rounds;
	bool fired = m_roundsWas >= 0 && rounds >= 0 && rounds < m_roundsWas;
	m_roundsWas = rounds;
	// A joined game's shots aren't known before the server's state comes:
	// the held fire input fires at the weapon's rate (fire.roundsPerMinute)
	// as the client's own prediction.
	if (m_remote && me.firing && weaponTemplate) {
		const float perMinute = number(*weaponTemplate, "fire.roundsPerMinute", 600);
		const int interval = std::max(1, static_cast<int>(std::lround(1800.0f / std::max(perMinute, 1.0f))));
		if (m_tick - m_lastRemoteShot >= interval) {
			fired = true;
			m_lastRemoteShot = m_tick;
		}
	}
	// A shot's recoil force (0x005dd3f0): recoilForceUp, times zoomModifier
	// while zoomed in (0x005dd090).
	if (fired && m_recoil.hasRecoilForce && !me.vehicle) {
		m_recoilForce = m_recoilForceUp.sample(m_random) * (m_zoom.level() > 0 ? m_recoil.zoomModifier : 1.0f);
		m_lastShotTick = m_tick;
	}
	if (m_recoil.hasRecoilForce) {
		m_recoilNow = anim::recoilAt(m_recoilGraph, m_tick - m_lastShotTick, m_recoilForce);
	}
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
	const int pose = me.vehicle ? 1 : me.stance >= 0 && me.stance < 3 ? me.stance : 0;
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
	const physics::Vec3 v {me.velocity[0], me.velocity[1], me.velocity[2]};
	const float speed = std::sqrt(v.x * v.x + v.z * v.z);
	const float yaw = me.bodyYaw * kDegrees;
	input.speed = speed;
	if (speed > 0.01f) {
		input.forward = (v.x * std::sin(yaw) + v.z * std::cos(yaw)) / speed;
		input.side = (v.x * std::cos(yaw) - v.z * std::sin(yaw)) / speed;
	}
	input.movementSeconds = seconds;
	input.pose = me.stance;
	if (reloading) {
		input.messages |= 2;
	}
	if (m_zoom.level() > 0) {
		input.messages |= 4;
	}
	if (fired) {
		input.pulses |= 1;
	}
	std::vector<anim::FirstPersonRig::Event> events;
	m_rig->update(input, seconds, &events);
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
		springInput.recoil = m_recoilNow;
		const float zoomedFov = m_zoom.factor(m_zoomSettings);
		springInput.zoom = zoomedFov > 0 && zoomedFov < m_baseFov
			? std::clamp((m_baseFov - m_fov.current) / (m_baseFov - zoomedFov), 0.0f, 1.0f)
			: 0.0f;
		m_springOffset = m_springState.update(*spring, springInput, seconds);
	}
	m_lastYaw = me.yaw;
	m_lastPitch = me.aimPitch;
	// The weapon's bundles follow its root on the animated first-person
	// skeleton (mesh1): the anchor goes there each tick.
	View eye;
	if (view(eye, 4.0f / 3.0f)) {
		const anim::Skeleton& skeleton = m_rig->skeleton();
		const int mesh1 = skeleton.find("mesh1");
		const anim::Matrix place = anim::firstPersonPlacement(skeleton, m_rig->pose(), m_rig->cameraNode(),
			eye.position, eye.yaw, eye.pitch, m_springOffset);
		const anim::Matrix part = mesh1 >= 0 ? anim::multiply(m_rig->pose()[static_cast<size_t>(mesh1)], place) : place;
		physics::Mat3 rotation;
		for (int r = 0; r < 3; ++r) {
			rotation.rows[static_cast<size_t>(r)] = {part[static_cast<size_t>(r * 4)],
				part[static_cast<size_t>(r * 4 + 1)], part[static_cast<size_t>(r * 4 + 2)]};
		}
		m_effects.setAnchor(kWeaponAnchor, {part[12], part[13], part[14]}, rotation);
	}
	// The muzzle flash: the weapon's child at its place on the weapon, the
	// zoomed one while zoomed in with disableMuzzleWhenZoomed; the systems
	// shown in first person.
	const Muzzle& muzzle = m_zoomSettings.disableMuzzle && zoomed() ? m_zoomMuzzle : m_muzzle;
	if (fired && firstPerson && !muzzle.effect.empty()) {
		const float* o = muzzle.offset.data();
		m_effects.spawnAttached(muzzle.effect, kWeaponAnchor, {o[0], o[1], o[2]}, {},
			effects::EffectManager::View::FirstPerson);
	}
	// The weapon's first-person effect events (0x2c + N) start its bundles
	// with bit 9 + N (0x0061a4e0); the soldier's first-person system has no
	// events in the shipped data (its sound events would be the soldier's,
	// 0x00587b90).
	for (const anim::FirstPersonRig::Event& event : events) {
		if (event.weapon && event.type == anim::Bundle::EffectEvent && event.id >= 0 && event.id < 10 && firstPerson) {
			startWeaponEffects(event.id);
		}
	}
}

void PlayerCamera::startWeaponEffects(int effect)
{
	const uint32_t bit = 1u << (9 + effect);
	for (const WeaponBundle& bundle : m_weaponBundles) {
		if (bundle.startOnEffects & bit) {
			m_effects.spawnAttached(bundle.name, kWeaponAnchor, {bundle.offset[0], bundle.offset[1], bundle.offset[2]}, {},
				effects::EffectManager::View::FirstPerson);
		}
	}
}

bool PlayerCamera::view(View& view, float aspect) const
{
	if (!insideView()) {
		return false;
	}
	Me me;
	if (!current(me) || !me.alive) {
		return false;
	}
	if (me.vehicle) {
		if (m_mode != game::camera::Mode::Inside || !seatCamera(view)) {
			return false;
		}
		view.fovY = aspectFov(view.fovY, aspect);
		return true;
	}
	// The soldier camera: its local position is (0, eye, 0) in the body's
	// frame, turned by the look (the body's yaw plus the head's twist) and
	// the aim's pitch (0x005890a0: Rx(pitch) x Ry(yaw), slot 0xe8).
	view.position = {me.origin[0], me.origin[1] + m_eye.current, me.origin[2]};
	view.yaw = me.yaw * kDegrees;
	view.pitch = std::clamp(me.aimPitch, -85.0f, 85.0f) * kDegrees;
	view.fovY = aspectFov(m_fov.current, aspect);
	return true;
}

// A seat's camera (Player::getCamera 0x005948e0 -> the seat's 0x005c7cb0):
// the seat's cameras by CameraId (0x0058bdb0 maps the seat's subtree, its
// nested seats left out), id 0, or 1 while ducking (0x005c7cf0 selects
// camera 1 when the seat has more than one); placed by the template tree
// (the guns' bundles turned as the game turns them), its insideFOV.

bool PlayerCamera::findSeatCamera(SeatView& out) const
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
	const int wanted = ducking() ? 1 : 0;
	// Depth-first through the tree: find the seat, then its cameras.
	struct Found {
		bool ok = false;
		render::Mat4 local;
		const world::ObjectTemplate* camera = nullptr;
		std::vector<render::Mat4> path; // the root's .. the camera's parent's
	} found, first;
	const world::ObjectTemplate* seatTemplate = nullptr;
	std::vector<render::Mat4> path;
	std::function<void(const world::ObjectTemplate&, const render::Mat4&, bool, int)> walk =
		[&](const world::ObjectTemplate& t, const render::Mat4& local, bool inSeat, int depth) {
			path.push_back(local);
			struct Pop {
				std::vector<render::Mat4>& path;
				~Pop() { path.pop_back(); }
			} pop {path};
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
					const Found here {true, m, c, path};
					if (!first.ok) {
						first = here;
					}
					if (static_cast<int>(number(*c, "CameraId", 0)) == wanted) {
						found = here;
						return;
					}
					continue;
				}
				const bool entering = seat && c->name() == seatName;
				if (entering) {
					seatTemplate = c;
				}
				walk(*c, m, inSeat || entering, depth + 1);
				if (found.ok) {
					return;
				}
			}
		};
	const world::ObjectTemplate& root = *vehicle->objectTemplate();
	if (root.name() == seatName) {
		seatTemplate = &root;
	}
	walk(root, render::Mat4 {}, root.name() == seatName, 0);
	if (!found.ok) {
		found = first;
	}
	if (!found.ok) {
		return false;
	}
	out.vehicle = object;
	out.seat = seatTemplate;
	out.camera = found.camera;
	const render::Mat4 place = placement(vehicle->position(), vehicle->rotation());
	out.world = found.local * place;
	out.chain.clear();
	out.chain.push_back(out.world);
	for (auto it = found.path.rbegin(); it != found.path.rend(); ++it) {
		out.chain.push_back(*it * place);
	}
	return true;
}

bool PlayerCamera::seatCamera(View& view) const
{
	SeatView seat;
	if (!findSeatCamera(seat)) {
		return false;
	}
	view.position = {seat.world.at(3, 0), seat.world.at(3, 1), seat.world.at(3, 2)};
	anglesOf(seat.world, view.yaw, view.pitch);
	view.fovY = number(*seat.camera, "insideFOV", 1.1f);
	view.roll = shakeDegrees() * kDegrees;
	return true;
}

bool PlayerCamera::ducking() const
{
	if (!m_crouch || !m_alive) {
		return false;
	}
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(m_round.localPlayer(), me) || !me.vehicle) {
		return false;
	}
	const auto seats = m_round.vehicleSeats(me.vehicle);
	if (me.seat < 0 || static_cast<size_t>(me.seat) >= seats.size()) {
		return false;
	}
	// The seat's template allowDucking (+0x468, read by 0x005ca2e0).
	const world::ObjectTemplate* seat = m_round.templates().find(seats[static_cast<size_t>(me.seat)].name);
	return seat && number(*seat, "allowDucking", 0) != 0;
}

bool PlayerCamera::duckingDisablesInput() const
{
	if (!ducking()) {
		return false;
	}
	game::GameWorld::PlayerInfo me;
	m_round.playerInfo(m_round.localPlayer(), me);
	const auto seats = m_round.vehicleSeats(me.vehicle);
	const world::ObjectTemplate* seat = m_round.templates().find(seats[static_cast<size_t>(me.seat)].name);
	return seat && number(*seat, "disableInputWhileDucking", 1) != 0; // +0x469, 1 by default
}

bool PlayerCamera::cockpit(Cockpit& cockpit) const
{
	if (!insideView()) {
		return false;
	}
	SeatView seat;
	if (!findSeatCamera(seat)) {
		return false;
	}
	// 0x00612880, first person (camera modes 0 and 1): the camera's
	// cockpitSubGeom / cockpitLod unless -1, else the seat's.
	const int cameraGeometry = static_cast<int>(number(*seat.camera, "cockpitSubGeom", -1));
	const int cameraLod = static_cast<int>(number(*seat.camera, "cockpitLod", -1));
	cockpit.vehicle = seat.vehicle;
	cockpit.subGeometry = cameraGeometry != -1 ? cameraGeometry
		: seat.seat                            ? static_cast<int>(number(*seat.seat, "cockpitSubGeom", 0))
											   : 0;
	cockpit.lod = cameraLod != -1 ? cameraLod : seat.seat ? static_cast<int>(number(*seat.seat, "cockpitLod", 0)) : 0;
	return true;
}

std::vector<PlayerCamera::Model> PlayerCamera::models() const
{
	std::vector<Model> out;
	View eye;
	Me me;
	if (!m_rig || !view(eye, 4.0f / 3.0f) || !current(me) || me.vehicle) {
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
	if (!insideView() || object == 0) {
		return false;
	}
	Me me;
	if (!current(me) || !me.alive) {
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
