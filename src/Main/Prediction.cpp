#include "Prediction.h"

#include "Game/GameWorld.h"
#include "World/ObjectTemplate.h"

#include <cmath>
#include <cstdlib>

namespace dice::hfe::host {

namespace {

constexpr float kTick = 1.0f / 30.0f;
constexpr size_t kHistory = 128; // moves remembered past the last record (four seconds)

float setting(const world::ObjectTemplate* t, const char* key)
{
	if (!t) {
		return 0;
	}
	const auto it = t->settings().find(key);
	return it == t->settings().end() ? 0.0f : static_cast<float>(std::atof(it->second.c_str()));
}

} // namespace

void SoldierPrediction::start(const world::ObjectTemplate* soldier, int32_t lastMove, const net::NetworkableState& state,
	const net::Vec3f& feet)
{
	m_control = {};
	m_body = {};
	// As GameWorld::stepSoldier sets a spawned soldier up: the stamina from
	// the template (0x00645790) and the jump's loss, the static objects'
	// soldier collision; a client runs no segment test (game logic slot
	// 0x144 is the server's).
	m_control.stamina().configure(setting(soldier, "sprintDissipationTime"), setting(soldier, "sprintRecoverTime"),
		setting(soldier, "sprintLimit"));
	m_control.jumpLoss = setting(soldier, "sprintLossAtJump");
	m_body.world = &m_round.staticCollision();
	m_body.server = false;
	m_body.friction = m_round.soldierFriction();
	m_active = true;
	// The moves sent since the record's are stepped on top of it.
	const Stats before = m_stats;
	reconcile(lastMove, state, feet);
	const uint32_t replayed = m_stats.replayed;
	m_stats = before; // a start corrects nothing
	m_stats.replayed = replayed;
	++m_stats.starts;
}

net::Vec3f SoldierPrediction::feet() const
{
	return {m_body.position.x, m_body.position.y - 1.0f, m_body.position.z};
}

net::Vec3f SoldierPrediction::velocity() const { return {m_body.velocity.x, m_body.velocity.y, m_body.velocity.z}; }

void SoldierPrediction::apply(const net::NetworkableState& s, const net::Vec3f& feet)
{
	// What 0x005fcdb0 sets in mode 3: the node's position (the object's,
	// a metre under the physics origin), velocity (slot 0x5c),
	// acceleration (0x78) and velocity change (0xc8); the angles (+0x25c,
	// +0x278, +0x280, +0x270), the eased throttle and strafe (+0x264,
	// +0x260), the stamina and sprinting (PCO vtable 0x10c, 0x104).
	const uint32_t m = s.mask;
	const net::Vec3f at = (m & 1) && s.hasPosition ? s.position : feet;
	m_body.position = {at.x, at.y + 1.0f, at.z};
	if (m & 0x100) {
		m_body.velocity = {s.velocity.x, s.velocity.y, s.velocity.z};
	}
	if (m & 0x200) {
		m_body.acceleration = {s.soldierAcceleration.x, s.soldierAcceleration.y, s.soldierAcceleration.z};
	}
	if (m & 0x80000) {
		m_body.impulse = {s.soldierImpulse.x, s.soldierImpulse.y, s.soldierImpulse.z};
	}
	if (m & 2) {
		m_body.yawDegrees = s.yaw;
	}
	m_control.restore((m & 0x800) ? s.soldierLean[0] : m_control.throttle(), (m & 0x1000) ? s.soldierLean[1] : m_control.strafe(),
		(m & 4) ? s.pitch : m_control.twist(), (m & 8) ? s.roll : m_control.catchUp());
	if (m & 0x10) {
		m_input.pitch = s.aimPitch;
	}
	if ((m & 0x8000) && s.stamina >= 0) {
		m_control.stamina().restore(s.stamina, s.sprinting);
	}
}

void SoldierPrediction::tick(const net::PlayerMove& move)
{
	game::SoldierInput input = net::soldierInputFromMove(move, m_input);
	if (!m_round.ground().height) {
		return;
	}
	// GameWorld::stepSoldier's pose rules: swimming (the response's +0xfa)
	// takes the swimming pose and stops strafing.
	if (m_body.swimming()) {
		input.pose = game::SoldierPose::Swim;
		input.strafe = 0;
	} else if (input.pose == game::SoldierPose::Swim) {
		input.pose = game::SoldierPose::Stand;
	}
	m_body.stance = static_cast<int>(input.pose);
	m_control.update(input, m_body, kTick);
	m_body.step(kTick, m_round.ground());
}

void SoldierPrediction::step(const net::PlayerMove& move)
{
	if (m_active) {
		tick(move);
		++m_stats.steps;
	} else {
		net::soldierInputFromMove(move, m_input); // the keys held, as the server's
	}
	// Without a soldier the moves are kept too: the first record's move is
	// one of them, and those after it are stepped on top of its state.
	m_history.push_back({move.index, move, m_input, m_control, m_body, m_active});
	while (m_history.size() > kHistory) {
		m_history.pop_front();
	}
}

void SoldierPrediction::reconcile(int32_t lastMove, const net::NetworkableState& state, const net::Vec3f& feet)
{
	if (!m_active) {
		return;
	}
	++m_stats.records;
	const net::Vec3f before = this->feet();
	m_lastMove = lastMove;
	while (!m_history.empty() && m_history.front().move < lastMove) {
		m_history.pop_front();
	}
	const bool known = !m_history.empty() && m_history.front().move == lastMove;
	if (known && m_history.front().stepped) {
		// The state that move left, corrected by the server's. It stays
		// remembered: the next record may be for the same move again (the
		// server had no new move for a tick and stepped the soldier on).
		const Snapshot& at = m_history.front();
		m_control = at.control;
		m_body = at.body;
		m_input = at.inputState;
	} else if (!known) {
		++m_stats.unknownMoves; // the server's state, and every move remembered on top of it
	}
	apply(state, feet);
	if (known) {
		Snapshot& at = m_history.front();
		at.control = m_control;
		at.body = m_body;
		at.inputState = m_input;
		at.stepped = true;
	}
	// The moves the server hasn't taken yet, again.
	for (size_t i = known ? 1 : 0; i < m_history.size(); ++i) {
		Snapshot& next = m_history[i];
		tick(next.input);
		next.inputState = m_input;
		next.control = m_control;
		next.body = m_body;
		next.stepped = true;
		++m_stats.replayed;
	}
	const net::Vec3f after = this->feet();
	const float dx = after.x - before.x, dy = after.y - before.y, dz = after.z - before.z;
	m_stats.lastCorrection = std::sqrt(dx * dx + dy * dy + dz * dz);
	m_stats.maxCorrection = std::max(m_stats.maxCorrection, m_stats.lastCorrection);
	m_stats.sumCorrection += m_stats.lastCorrection;
	if (m_stats.lastCorrection > 0.01f) {
		++m_stats.corrections;
	}
}

physics::Mat3 rotationFromQuaternion(const net::Quat& q)
{
	// The column matrix R of the quaternion; the rows of physics' matrix
	// are R's columns (it acts on row vectors).
	const float x = q.x, y = q.y, z = q.z, w = q.w;
	const float r[3][3] = {
		{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
		{2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
		{2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
	};
	physics::Mat3 m;
	for (int j = 0; j < 3; ++j) {
		m.rows[static_cast<size_t>(j)] = {r[0][j], r[1][j], r[2][j]};
	}
	return m;
}

bool VehiclePrediction::start(const world::ObjectTemplate* vehicle, int32_t lastMove, const net::NetworkableState& root,
	const net::Vec3f& position)
{
	m_vehicle.reset();
	m_template = vehicle;
	if (!vehicle) {
		return false;
	}
	const physics::Mat3 rotation = root.hasOrientation ? rotationFromQuaternion(root.orientation) : physics::Mat3 {};
	m_vehicle = m_round.makeLoneVehicle(*vehicle, {position.x, position.y, position.z}, rotation);
	if (!m_vehicle) {
		return false;
	}
	const Stats before = m_stats;
	reconcile(lastMove, root, position);
	const uint32_t replayed = m_stats.replayed;
	m_stats = before;
	m_stats.replayed = replayed;
	++m_stats.starts;
	return true;
}

void VehiclePrediction::stop()
{
	m_vehicle.reset();
	m_template = nullptr;
}

net::Vec3f VehiclePrediction::position() const
{
	if (!m_vehicle) {
		return {};
	}
	const physics::Vec3& p = m_vehicle->body().position;
	return {p.x, p.y, p.z};
}

net::Vec3f VehiclePrediction::velocity() const
{
	if (!m_vehicle) {
		return {};
	}
	const physics::Vec3& v = m_vehicle->body().velocity;
	return {v.x, v.y, v.z};
}

physics::Mat3 VehiclePrediction::rotation() const { return m_vehicle ? m_vehicle->body().rotation : physics::Mat3 {}; }

void VehiclePrediction::apply(const net::NetworkableState& s, const net::Vec3f& position)
{
	// What 0x005ffec0 sets on the root's physics node in mode 3: the
	// position (the record's when the group carries none), the
	// orientation, the velocity and the angular velocity.
	physics::RigidBody& body = m_vehicle->body();
	const net::Vec3f at = s.hasPosition ? s.position : position;
	body.position = {at.x, at.y, at.z};
	if (s.hasOrientation) {
		body.rotation = rotationFromQuaternion(s.orientation);
	}
	if (s.hasVelocity) {
		body.velocity = {s.velocity.x, s.velocity.y, s.velocity.z};
	}
	if (s.hasAngularVelocity) {
		body.angularVelocity = {s.angularVelocity.x, s.angularVelocity.y, s.angularVelocity.z};
	}
}

void VehiclePrediction::tick(const net::PlayerMove& move)
{
	// The server's driver: the move as the soldier's input
	// (net::soldierInputFromMove), the seat's inputs from it
	// (GameWorld::driverInput: no EMP or damage scaling on the client),
	// one tick of the vehicle.
	const game::SoldierInput input = net::soldierInputFromMove(move, m_input);
	m_round.stepLoneVehicle(*m_vehicle, game::GameWorld::driverInput(input, 1, m_lastLookPitch), kTick);
	m_lastLookPitch = input.aimPitch;
}

void VehiclePrediction::step(const net::PlayerMove& move)
{
	if (m_vehicle) {
		tick(move);
		++m_stats.steps;
	} else {
		const game::SoldierInput input = net::soldierInputFromMove(move, m_input);
		m_lastLookPitch = input.aimPitch;
	}
	Snapshot snapshot;
	snapshot.move = move.index;
	snapshot.input = move;
	snapshot.inputState = m_input;
	snapshot.lastLookPitch = m_lastLookPitch;
	snapshot.stepped = m_vehicle != nullptr;
	if (m_vehicle) {
		snapshot.state = m_vehicle->save();
	}
	m_history.push_back(std::move(snapshot));
	while (m_history.size() > kHistory) {
		m_history.pop_front();
	}
}

void VehiclePrediction::reconcile(int32_t lastMove, const net::NetworkableState& root, const net::Vec3f& position)
{
	if (!m_vehicle) {
		return;
	}
	++m_stats.records;
	const net::Vec3f before = this->position();
	while (!m_history.empty() && m_history.front().move < lastMove) {
		m_history.pop_front();
	}
	const bool known = !m_history.empty() && m_history.front().move == lastMove;
	if (known && m_history.front().stepped) {
		const Snapshot& at = m_history.front();
		m_vehicle->restore(at.state);
		m_input = at.inputState;
		m_lastLookPitch = at.lastLookPitch;
	} else if (!known) {
		++m_stats.unknownMoves;
	}
	apply(root, position);
	if (known) {
		Snapshot& at = m_history.front();
		at.state = m_vehicle->save();
		at.inputState = m_input;
		at.lastLookPitch = m_lastLookPitch;
		at.stepped = true;
	}
	for (size_t i = known ? 1 : 0; i < m_history.size(); ++i) {
		Snapshot& next = m_history[i];
		tick(next.input);
		next.inputState = m_input;
		next.lastLookPitch = m_lastLookPitch;
		next.state = m_vehicle->save();
		next.stepped = true;
		++m_stats.replayed;
	}
	const net::Vec3f after = this->position();
	const float dx = after.x - before.x, dy = after.y - before.y, dz = after.z - before.z;
	m_stats.lastCorrection = std::sqrt(dx * dx + dy * dy + dz * dz);
	m_stats.maxCorrection = std::max(m_stats.maxCorrection, m_stats.lastCorrection);
	m_stats.sumCorrection += m_stats.lastCorrection;
	if (m_stats.lastCorrection > 0.01f) {
		++m_stats.corrections;
	}
}

} // namespace dice::hfe::host
