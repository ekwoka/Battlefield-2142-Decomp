#include "Game/Director.h"

#include "Game/Bots.h"
#include "Game/GameWorld.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace dice::hfe::game::director {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRadians = kPi / 180.0f;

float fade(float since, float span) { return since < span ? 1.0f - since / span : 0.0f; }

float distance(const physics::Vec3& a, const physics::Vec3& b) { return std::sqrt(physics::lengthSquared(a - b)); }

float flatDistance(const physics::Vec3& a, const physics::Vec3& b)
{
	const float dx = a.x - b.x, dz = a.z - b.z;
	return std::sqrt(dx * dx + dz * dz);
}

// The share of the way an eased value moves in `seconds` with time constant `tau`.
float ease(float seconds, float tau) { return tau <= 0 ? 1.0f : 1.0f - std::exp(-seconds / tau); }

float wrapDegrees(float degrees) { return std::remainder(degrees, 360.0f); }

physics::Vec3 direction(float yawDegrees, float pitchDegrees)
{
	const float yaw = yawDegrees * kRadians, pitch = pitchDegrees * kRadians;
	return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

void keepAbove(physics::Vec3& at, const camera::Ground& ground, float clearance)
{
	at.y = std::max(at.y, std::max(ground.terrainAt(at.x, at.z), ground.seaAt(at.x, at.z)) + clearance);
}

// The eye over the soldier's feet in each pose (the soldier camera's
// poseCameraPosition over the body's origin a metre up, EyeHeight).
float eyeOverFeet(SoldierPose pose)
{
	switch (pose) {
	case SoldierPose::Crouch: return 0.95f;
	case SoldierPose::Prone: return 0.37f;
	default: return 1.47f;
	}
}

bool isFlying(GameWorld& world, uint32_t vehicle)
{
	const world::Object* o = vehicle ? world.objects().find(vehicle) : nullptr;
	if (!o || !o->objectTemplate()) {
		return false;
	}
	const int category = GameWorld::vehicleCategory(*o->objectTemplate());
	return category == 2 || category == 3; // VCAir, VCHelicopter
}

} // namespace

const char* modeName(CameraMode mode)
{
	switch (mode) {
	case CameraMode::FirstPerson: return "first-person";
	case CameraMode::Wide: return "wide";
	default: return "chase";
	}
}

float interest(const Sample& s)
{
	if (!s.alive) {
		return 0;
	}
	float score = 2;
	score += 3 * fade(s.sinceFired, 3);
	score += 3 * fade(s.sinceHit, 4);
	score += 4 * fade(s.sinceDamage, 4);
	score += 5 * fade(s.sinceKill, 6);
	score += s.enemyInRange ? 3.0f : s.hasTarget ? 1.0f : 0.0f;
	// Vehicle and aircraft fights: on the move (a fixed gun or a parked
	// vehicle firing scores as a soldier does).
	if (s.vehicle && !s.ownTitan && (s.flying || s.speed >= 2) && (s.enemyInRange || s.sinceFired < 3)) {
		score += s.flying ? 3.5f : 2.0f; // vehicle and aircraft fights
	}
	if (s.capturing) {
		score += 2;
	}
	if (s.titanAssault) {
		score += 2.5f;
	}
	if (s.titanCore) {
		score += 2;
	}
	score -= 0.3f * std::min(s.idleFor, 10.0f);
	if (!s.hasTarget && (s.speed < 0.75f || s.idleBehaviour)) {
		score -= 1; // barely moving, or the AI idles
	}
	if (s.vehicle && !s.hasTarget && s.speed < 2) {
		score -= 1.5f; // sitting in a seat with nothing to shoot at
	}
	if (s.inSpawn) {
		score -= 2;
	}
	if (s.ownTitan) {
		score -= 1;
	}
	return std::max(score, 0.01f);
}

bool engaged(const Sample& s)
{
	return s.alive && (s.enemyInRange || s.sinceFired < 3 || s.sinceHit < 3 || s.sinceDamage < 4 || s.sinceKill < 5);
}

// --- Observer

Observer::Observer(GameWorld& world, const BotManager* bots) : m_world(world), m_bots(bots) {}

const Sample* Observer::find(int player) const
{
	for (const Sample& s : m_samples) {
		if (s.player == player) {
			return &s;
		}
	}
	return nullptr;
}

void Observer::observe(float now)
{
	std::vector<int> damaged;
	std::unordered_map<int, int> targets;
	for (const GameWorld::PlayerInfo& p : m_world.playerList()) {
		if (!p.ai) {
			continue; // the spectator, and any human
		}
		Memory& m = m_memory[p.id];
		if (p.alive && !m.alive) {
			m.aliveSince = now;
			m.hitPoints = -1;
			m.idleSince = -1;
			m.gunShots = p.gunShots;
			m.hits = p.hitDirectionHits;
		}
		if (!p.alive && m.alive) {
			m.lastHit = now;
			if (p.killer >= 0 && p.killer != p.id) {
				Memory& killer = m_memory[p.killer];
				killer.lastKill = killer.lastDamage = now;
			}
		}
		m.alive = p.alive;
		if (p.kills < m.kills) {
			m.kills = p.kills; // a new round
		}
		if (!p.alive) {
			continue;
		}
		const uint32_t object = p.vehicle ? p.vehicle : p.soldier;
		if (p.firing || (m_bots && m_bots->lastInputFor(p.id).fire) || p.gunShots != m.gunShots) {
			m.lastFired = now;
		}
		m.gunShots = p.gunShots;
		if (p.hitDirectionHits != m.hits) {
			m.lastHit = now;
		}
		m.hits = p.hitDirectionHits;
		const float hp = object ? m_world.hitPoints(object) : -1;
		if (object == m.object && m.hitPoints >= 0 && hp >= 0 && hp < m.hitPoints - 0.01f) {
			m.lastHit = now;
			damaged.push_back(p.id);
		}
		m.object = object;
		m.hitPoints = hp;
		if (const world::Object* o = object ? m_world.objects().find(object) : nullptr) {
			m.position = {o->position().x, o->position().y, o->position().z};
		} else {
			m.position = p.position;
		}
		if (p.kills > m.kills) {
			m.lastKill = m.lastDamage = now;
		}
		m.kills = p.kills;
		if (m_bots) {
			targets[p.id] = m_bots->target(p.id);
		}
	}
	// Damage dealt: a bot that fired lately at a target that just lost hit points.
	for (const int victim : damaged) {
		for (const auto& [shooter, target] : targets) {
			Memory& m = m_memory[shooter];
			if (target == victim && now - m.lastFired < 1.5f) {
				m.lastDamage = now;
			}
		}
	}
}

void Observer::sample(float now)
{
	m_samples.clear();
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	const std::vector<GameWorld::PlayerInfo> players = m_world.playerList();
	const auto info = [&players](int id) -> const GameWorld::PlayerInfo* {
		for (const auto& p : players) {
			if (p.id == id) {
				return &p;
			}
		}
		return nullptr;
	};
	for (const GameWorld::PlayerInfo& p : players) {
		if (!p.ai) {
			continue;
		}
		Memory& m = m_memory[p.id];
		Sample s;
		s.player = p.id;
		s.name = p.name;
		s.team = p.team;
		s.alive = p.alive && m.alive;
		s.soldier = p.soldier;
		s.vehicle = p.vehicle;
		s.seat = p.seat;
		s.killer = p.killer;
		s.position = m.position;
		s.focus = m.position;
		if (!s.alive) {
			m.sampledAt = -1;
			m_samples.push_back(s);
			continue;
		}
		s.sinceFired = now - m.lastFired;
		s.sinceHit = now - m.lastHit;
		s.sinceDamage = now - m.lastDamage;
		s.sinceKill = now - m.lastKill;
		s.aliveFor = now - m.aliveSince;
		s.flying = isFlying(m_world, p.vehicle);
		s.ownTitan = p.vehicle && p.vehicle == m_world.titanOf(p.team);
		const int target = m_bots ? m_bots->target(p.id) : -1;
		const GameWorld::PlayerInfo* enemy = target >= 0 ? info(target) : nullptr;
		s.hasTarget = enemy && enemy->alive;
		if (s.hasTarget) {
			const physics::Vec3 at = enemy->vehicle ? m_memory[target].position : enemy->position;
			const float d = distance(s.position, at);
			float range = 120;
			if (s.flying) {
				range = 400;
			} else if (p.vehicle) {
				range = 250;
			} else if (const float preferred = m_bots->preferredRange(p.id); preferred > 0) {
				range = std::max(preferred * 1.6f, 40.0f);
			}
			s.enemyInRange = d <= range && m_bots->sees(p.id, target);
			if (d < 150) {
				s.focus = (s.position + at) * 0.5f;
			}
		}
		const std::string behaviour = m_bots ? m_bots->behaviour(p.id) : std::string();
		s.idleBehaviour = behaviour.empty() || behaviour == "Idle" || behaviour == "Null";
		for (const auto& cp : points) {
			if (cp.capturable && cp.team != p.team && !s.flying && flatDistance(s.position, cp.position) < std::max(cp.radius, 5.0f)) {
				s.capturing = true;
			}
		}
		const uint32_t enclosing = m_world.enclosingTitan(p.id);
		for (const auto& titan : titans) {
			if (titan.team == p.team || titan.wreck || titan.coreDestroyed) {
				continue;
			}
			const bool inside = enclosing == titan.object || m_world.titanHolding(s.position) == titan.object;
			if (inside || flatDistance(s.position, titan.position) < titan.radius + 30) {
				s.titanAssault = true;
			}
			if (inside && titan.coreExposed) {
				s.titanCore = true;
			}
		}
		bool atBase = false;
		for (const auto& cp : points) {
			if (!cp.capturable && cp.team == p.team && flatDistance(s.position, cp.position) < std::max(cp.radius, 40.0f)) {
				atBase = true;
			}
		}
		s.inSpawn = s.aliveFor < 5 || (atBase && !s.hasTarget);
		// The speed over the last sample.
		if (m.sampledAt >= 0 && now > m.sampledAt) {
			s.speed = distance(s.position, m.sampledPosition) / (now - m.sampledAt);
		}
		m.sampledPosition = s.position;
		// Idle: nothing to fight and nothing to take, moving or not (standing
		// still costs more in interest()).
		const bool idle = !s.hasTarget && s.sinceFired >= 2 && s.sinceHit >= 2 && !s.capturing && !s.titanAssault;
		if (idle) {
			if (m.idleSince < 0) {
				m.idleSince = m.sampledAt >= 0 ? m.sampledAt : now;
			}
			s.idleFor = now - m.idleSince;
		} else {
			m.idleSince = -1;
		}
		m.sampledAt = now;
		m_samples.push_back(s);
	}
}

// --- ShotDirector

ShotDirector::ShotDirector(Settings settings, uint32_t seed) : m_settings(settings), m_seed(seed ? seed : 1) {}

float ShotDirector::random()
{
	// xorshift32: the director's own stream, apart from the game's.
	m_seed ^= m_seed << 13;
	m_seed ^= m_seed >> 17;
	m_seed ^= m_seed << 5;
	return static_cast<float>(m_seed % 100000u) / 100000.0f;
}

const Sample* ShotDirector::best(const std::vector<Sample>& samples, int exclude, float& score) const
{
	const Sample* chosen = nullptr;
	score = 0;
	for (const Sample& s : samples) {
		if (!s.alive || s.player == exclude) {
			continue;
		}
		float value = interest(s);
		if (std::find(m_recent.begin(), m_recent.end(), s.player) != m_recent.end()) {
			value -= m_settings.variety + m_settings.varietyShare * value;
		}
		if (!chosen || value > score) {
			chosen = &s;
			score = value;
		}
	}
	return chosen;
}

CameraMode ShotDirector::chooseMode(const Sample& s)
{
	if (random() < m_settings.wideShare && m_mode != CameraMode::Wide) {
		return CameraMode::Wide;
	}
	if (!s.vehicle && random() < m_settings.firstPersonShare) {
		return CameraMode::FirstPerson;
	}
	return CameraMode::Chase;
}

Cut ShotDirector::cutTo(float now, const Sample* to, const char* reason)
{
	Cut cut;
	cut.time = now;
	cut.previous = m_subject;
	cut.subject = to->player;
	cut.reason = reason;
	cut.score = interest(*to);
	cut.engaged = engaged(*to);
	m_subject = to->player;
	m_mode = chooseMode(*to);
	cut.mode = m_mode;
	m_shotStart = now;
	m_deathAt = -1;
	m_recent.push_back(to->player);
	while (static_cast<int>(m_recent.size()) > std::max(m_settings.history, 0)) {
		m_recent.pop_front();
	}
	return cut;
}

std::optional<Cut> ShotDirector::update(float now, const std::vector<Sample>& samples)
{
	const bool forced = std::exchange(m_forced, false);
	const Sample* current = nullptr;
	for (const Sample& s : samples) {
		if (s.player == m_subject) {
			current = &s;
		}
	}
	float score = 0;
	if (!current) {
		const Sample* to = best(samples, -1, score);
		if (!to) {
			return std::nullopt;
		}
		return cutTo(now, to, m_subject < 0 ? "start" : "lost");
	}
	if (m_deathAt >= 0) {
		if (current->alive && !forced) {
			m_deathAt = -1; // back (a revive)
		} else if (now - m_deathAt < m_settings.deathHold && !forced) {
			return std::nullopt;
		} else {
			const Sample* to = best(samples, m_subject, score);
			return to ? std::optional<Cut>(cutTo(now, to, "death")) : std::nullopt;
		}
	}
	if (!current->alive) {
		m_deathAt = now; // the death view holds
		return std::nullopt;
	}
	const float age = now - m_shotStart;
	const float mine = interest(*current);
	const Sample* to = best(samples, m_subject, score);
	if (!to) {
		return std::nullopt;
	}
	if (forced) {
		return cutTo(now, to, "forced");
	}
	if (age >= m_settings.maxShot) {
		return cutTo(now, to, "max");
	}
	if (age >= m_settings.idleCut && current->idleFor >= m_settings.idleCut && to->idleFor < m_settings.idleCut && score >= mine + 1) {
		return cutTo(now, to, "idle");
	}
	if (age >= m_settings.minShot && score >= mine + std::max(m_settings.margin, m_settings.marginShare * mine)) {
		return cutTo(now, to, "better");
	}
	return std::nullopt;
}

// --- DirectorCamera

float DirectorCamera::fov(CameraMode mode)
{
	switch (mode) {
	case CameraMode::FirstPerson: return 1.0f;
	case CameraMode::Wide: return 0.8f;
	default: return 1.05f;
	}
}

camera::Look DirectorCamera::update(CameraMode mode, const Pose& pose, float seconds, const camera::Ground& ground)
{
	using camera::Frame;
	if (pose.inVehicle && mode == CameraMode::FirstPerson) {
		mode = CameraMode::Chase; // a seat's own camera isn't the director's: the chase
	}
	// What the camera turns with: the aim on foot, the hull in a vehicle.
	float yaw = pose.yaw, pitch = pose.pitch;
	if (pose.inVehicle) {
		const physics::Vec3 f = pose.vehicle.forward();
		yaw = std::atan2(f.x, f.z) / kRadians;
		pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f)) / kRadians;
	}
	const bool fresh = std::exchange(m_fresh, false);
	if (fresh) {
		m_yaw = yaw;
		m_pitch = pitch;
		m_focus = pose.focus;
		m_eyeHeight = pose.eye.y;
		m_orbit = yaw + 150;
	}
	camera::Look look;
	switch (mode) {
	case CameraMode::FirstPerson: {
		const float k = ease(seconds, kEyeSmoothing);
		m_yaw = wrapDegrees(m_yaw + wrapDegrees(yaw - m_yaw) * k);
		m_pitch += (pitch - m_pitch) * k;
		m_eyeHeight += (pose.eye.y - m_eyeHeight) * ease(seconds, kEyeSmoothing);
		look.eye = {pose.eye.x, m_eyeHeight, pose.eye.z};
		look.at = look.eye + direction(m_yaw, m_pitch);
		break;
	}
	case CameraMode::Chase: {
		const float k = ease(seconds, kChaseSmoothing);
		m_yaw = wrapDegrees(m_yaw + wrapDegrees(yaw - m_yaw) * k);
		// Only part of the pitch: the chase stays behind, not above or under.
		m_pitch += (pitch * (pose.inVehicle ? 0.6f : 0.35f) - m_pitch) * k;
		Frame frame;
		frame.rotation = physics::Mat3::fromYawPitchRoll({m_yaw, -m_pitch, 0});
		frame.position = pose.inVehicle ? pose.vehicle.position : pose.eye;
		const float radius = pose.inVehicle ? std::max(pose.radius * 1.3f, 5.0f) : 3.0f;
		if (fresh) {
			// Start in place, not easing out of the subject.
			m_follow.offset = frame.forward() * -radius + frame.up() * (radius * 0.3f);
		}
		look = camera::follow(m_follow, frame, radius, &pose.velocity, ground);
		if (!pose.inVehicle) {
			look.at = look.at + physics::Vec3 {0, 0.25f, 0};
		}
		break;
	}
	case CameraMode::Wide: {
		m_focus = m_focus + (pose.focus - m_focus) * ease(seconds, pose.flying ? 0.3f : kFocusSmoothing);
		m_orbit = wrapDegrees(m_orbit + kOrbitDegreesPerSecond * seconds);
		const float reach = pose.flying ? 70.0f : pose.inVehicle ? 40.0f : 28.0f;
		const float height = pose.flying ? 25.0f : 12.0f;
		look.eye = m_focus + physics::Vec3 {std::sin(m_orbit * kRadians) * reach, height, std::cos(m_orbit * kRadians) * reach};
		keepAbove(look.eye, ground, 3);
		look.at = m_focus + physics::Vec3 {0, 1, 0};
		break;
	}
	}
	m_last = look;
	return look;
}

void DirectorCamera::startDeath(const physics::Vec3& body, float radius)
{
	const physics::Vec3 forward = camera::normalized(m_last.at - m_last.eye);
	camera::deathStart(m_death, body, forward, std::max(radius, 1.0f));
}

camera::Look DirectorCamera::death(const physics::Vec3* body, const physics::Vec3* killer, float seconds, const camera::Ground& ground)
{
	m_last = camera::death(m_death, body, killer, seconds, ground);
	return m_last;
}

// --- Director

Director::Director(GameWorld& world, const BotManager* bots, Settings settings, uint32_t seed)
	: m_world(world), m_observer(world, bots), m_shots(settings, seed)
{
}

CameraMode Director::mode() const { return m_shots.mode(); }

camera::Ground Director::ground() const
{
	camera::Ground g;
	GameWorld& world = m_world;
	g.terrain = [&world](float x, float z) { return world.terrainHeight(x, z); };
	float sea = 0;
	if (m_world.waterLevel(sea)) {
		g.sea = [sea](float, float) { return sea; };
	}
	return g;
}

Pose Director::poseOf(const Sample& sample) const
{
	Pose pose;
	GameWorld::PlayerInfo info;
	if (!m_world.playerInfo(sample.player, info)) {
		return pose;
	}
	pose.eye = info.position + physics::Vec3 {0, eyeOverFeet(info.pose), 0};
	pose.yaw = info.yaw;
	pose.pitch = info.aimPitch;
	pose.velocity = info.velocity;
	pose.focus = sample.focus;
	const world::Object* v = info.vehicle ? m_world.objects().find(info.vehicle) : nullptr;
	if (v && (info.vehicle == m_world.titanOf(1) || info.vehicle == m_world.titanOf(2))) {
		// A Titan's gun: framed at the gunner along the gun (the hull's chase
		// would stand a hundred metres off), as a soldier's.
		float gunYaw = 0, gunPitch = 0;
		if (m_world.gunAim(info.vehicle, info.seat, gunYaw, gunPitch)) {
			pose.yaw = v->rotation().x + gunYaw;
			pose.pitch = gunPitch;
		}
		pose.eye = info.position + physics::Vec3 {0, 1.0f, 0};
		pose.velocity = {};
		return pose;
	}
	if (v) {
		pose.inVehicle = true;
		pose.vehicle.rotation = physics::Mat3::fromYawPitchRoll({v->rotation().x, v->rotation().y, v->rotation().z});
		pose.vehicle.position = {v->position().x, v->position().y, v->position().z};
		const float radius = m_world.objectRadius(info.vehicle);
		pose.radius = radius > 0 ? radius : 3.0f;
		pose.flying = sample.flying;
		pose.velocity = {};
	}
	return pose;
}

void Director::tick(float seconds)
{
	m_time += seconds;
	m_cutLastTick = false;
	m_observer.observe(m_time);
	// The subject's death decides at once (the hold starts now).
	GameWorld::PlayerInfo live;
	const bool subjectKnown = m_shots.subject() >= 0 && m_world.playerInfo(m_shots.subject(), live);
	if (subjectKnown && !live.alive && !m_shots.holdingDeath()) {
		m_nextSample = m_time;
	}
	if (m_time >= m_nextSample) {
		m_nextSample = m_time + m_shots.settings().sampleEvery;
		m_observer.sample(m_time);
		if (const std::optional<Cut> cut = m_shots.update(m_time, m_observer.samples())) {
			m_cuts.push_back(*cut);
			if (onCut) {
				onCut(*cut);
			}
			m_camera.cut();
			m_cutLastTick = true;
			m_dying = false;
		}
		if (const Sample* s = m_observer.find(m_shots.subject()); s && s->alive) {
			++decisions;
			engagedDecisions += engaged(*s) ? 1 : 0;
		}
	}
	const Sample* subject = m_observer.find(m_shots.subject());
	if (!subject) {
		return; // nobody to watch yet: the view stays
	}
	GameWorld::PlayerInfo info;
	const bool alive = m_world.playerInfo(subject->player, info) && info.alive;
	const camera::Ground g = ground();
	if (!alive) {
		// The death view: on the body (or the wreck), towards the killer.
		physics::Vec3 body = subject->position;
		if (!m_dying) {
			m_dying = true;
			m_body = subject->vehicle ? subject->vehicle : subject->soldier;
			const float radius = subject->vehicle ? m_world.objectRadius(subject->vehicle) : 1.0f;
			m_camera.startDeath(body, radius);
		}
		if (const world::Object* o = m_body ? m_world.objects().find(m_body) : nullptr) {
			body = {o->position().x, o->position().y, o->position().z};
		}
		GameWorld::PlayerInfo killer;
		physics::Vec3 at;
		const bool hasKiller = info.killer >= 0 && info.killer != info.id && m_world.playerInfo(info.killer, killer) && killer.alive;
		if (hasKiller) {
			const world::Object* k = m_world.objects().find(killer.vehicle ? killer.vehicle : killer.soldier);
			at = k ? physics::Vec3 {k->position().x, k->position().y, k->position().z} : killer.position;
		}
		m_look = m_camera.death(&body, hasKiller ? &at : nullptr, seconds, g);
		m_fov = DirectorCamera::fov(CameraMode::Chase);
		m_hasView = true;
		return;
	}
	m_dying = false;
	m_look = m_camera.update(m_shots.mode(), poseOf(*subject), seconds, g);
	m_fov = DirectorCamera::fov(info.vehicle && m_shots.mode() == CameraMode::FirstPerson ? CameraMode::Chase : m_shots.mode());
	m_hasView = true;
}

bool Director::view(camera::Look& look, float& fovY) const
{
	if (!m_hasView) {
		return false;
	}
	look = m_look;
	fovY = m_fov;
	return true;
}

std::vector<uint32_t> Director::hidden() const
{
	GameWorld::PlayerInfo info;
	if (m_shots.mode() != CameraMode::FirstPerson || m_dying || !m_world.playerInfo(m_shots.subject(), info) || !info.alive
		|| info.vehicle) {
		return {};
	}
	return {info.soldier, info.weapon, info.kit};
}

} // namespace dice::hfe::game::director
