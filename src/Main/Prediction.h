// A joined game's own soldier, predicted: the client steps its soldier
// with each move it sends, using the same soldier simulation the server
// runs (game::SoldierControl and physics::SoldierBody against the round's
// ground and static collision, as GameWorld::stepSoldier), and takes the
// server's word whenever a control-object record comes.
//
// BF2142.exe's ghost block reader (0x00580cd0) reads the record: the last
// move the server took (sign + 31 bits), the control object's state in mode
// 3 (the Soldier's reader 0x005fcdb0 with its last argument 3: the position,
// the physics node's velocity, acceleration and velocity change, the body's
// yaw, the look's twist and catch-up, the aim, the eased throttle and
// strafe, the stamina and sprinting) and the player's own state; then it
// hands the stream, the record's ghost, that move and the server's tick
// to the local player's interface 0x1d4c2 (`DAT_00a1b5d8` slot 0xc, then
// its slot 0x10 with 1/30 s, 0x005811ee-0x0058122a), which runs the
// moves the server hasn't taken yet again on top of that state. The
// replay's own steps weren't traced; this one follows what the state
// carries:
//   - each move sent is stepped once (one 1/30 s tick, as the server's
//     HostedGame takes one move a tick) and remembered with the state it
//     left behind;
//   - a record for move L puts the remembered state of L right where the
//     server's differs (every field the mode-3 state carries; the timers
//     and contacts it doesn't carry stay the client's own), then steps the
//     moves after L again.
// The correction (how far the prediction stood from the corrected one)
// is kept for diagnostics.
#pragma once

#include "Game/SoldierControl.h"
#include "Net/GameProtocol.h"
#include "Net/Networkables.h"
#include "Net/RemoteInput.h"
#include "Physics/SoldierPhysics.h"

#include <cstdint>
#include <deque>

namespace dice::hfe::world {
class ObjectTemplate;
}

namespace dice::hfe::game {
class GameWorld;
}

namespace dice::hfe::host {

class SoldierPrediction {
public:
	explicit SoldierPrediction(const game::GameWorld& round) : m_round(round) {}

	// Starts predicting a soldier of this template at the server's state
	// after `lastMove` (its feet, the record's position), with the moves
	// sent since then on top.
	void start(const world::ObjectTemplate* soldier, int32_t lastMove, const net::NetworkableState& state,
		const net::Vec3f& feet);
	void stop() { m_active = false; }
	bool active() const { return m_active; }

	// A move just sent (GameClient::onMoveSent): one tick of it.
	void step(const net::PlayerMove& move);
	// A control-object record: the server's state after move `lastMove`.
	void reconcile(int32_t lastMove, const net::NetworkableState& state, const net::Vec3f& feet);

	// The predicted soldier: its feet, yaw (the body's) and the look.
	net::Vec3f feet() const;
	net::Vec3f velocity() const;
	float yaw() const { return m_body.yawDegrees; }
	float lookYaw() const { return m_body.yawDegrees + m_control.twist(); }
	float aimPitch() const { return m_input.pitch; }
	bool onGround() const { return m_body.onGround(); }
	int32_t lastMove() const { return m_lastMove; }

	struct Stats {
		uint32_t steps = 0;        // moves stepped as they were sent
		uint32_t replayed = 0;     // moves stepped again after a record
		uint32_t starts = 0;       // soldiers started from a record (spawns)
		uint32_t records = 0;      // records taken after the start
		uint32_t unknownMoves = 0; // records for a move no longer (or not yet) remembered
		float lastCorrection = 0;  // metres the last record moved the present prediction
		float maxCorrection = 0;
		double sumCorrection = 0;
		uint32_t corrections = 0;  // records that moved it over 1 cm
	};
	const Stats& stats() const { return m_stats; }

private:
	struct Snapshot {
		int32_t move = 0;
		net::PlayerMove input;
		net::MoveInputState inputState; // after the move
		game::SoldierControl control;
		physics::SoldierBody body;
		bool stepped = false; // a predicted state (moves sent before the soldier carry only their input)
	};
	void tick(const net::PlayerMove& move);
	void apply(const net::NetworkableState& state, const net::Vec3f& feet);

	const game::GameWorld& m_round;
	bool m_active = false;
	game::SoldierControl m_control;
	physics::SoldierBody m_body;
	net::MoveInputState m_input;
	std::deque<Snapshot> m_history; // the moves after the last record's, oldest first
	int32_t m_lastMove = -1;
	Stats m_stats;
};

} // namespace dice::hfe::host
