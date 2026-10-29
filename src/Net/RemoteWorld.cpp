#include "RemoteWorld.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace dice::hfe::net {

RemotePlayer& RemoteWorld::playerSlot(int id)
{
	RemotePlayer& p = m_players[id];
	p.id = id;
	return p;
}

void RemoteWorld::clear()
{
	m_objects.clear();
	m_hidden.clear();
	m_players.clear();
	m_objectHints.clear();
	m_playerGhosts.clear();
	m_localPlayer = -1;
	m_hasControl = false;
	m_controlInLastBlock = m_controlStateValid = false;
	m_ghostParts.clear();
	m_gameState = 0;
	m_roundStarted = false;
	m_round = {};
	m_roundEnded = false;
	m_endOfRound = {};
	m_levels = 0;
	m_level = {};
	m_levelChanges.clear();
	m_dataBlocks.clear();
	m_inData = m_inString = false;
}

void RemoteWorld::applyDataBlock(const DataBlockChunk& chunk)
{
	// 0x0050a330 hands both parts to the client's transfer (0x00643510).
	if (chunk.start) {
		m_inData = true;
		m_dataType = chunk.type;
		m_dataSize = chunk.size;
		m_dataBuffer.clear();
		if (m_dataSize == 0) {
			m_dataBlocks[m_dataType].clear();
			m_inData = false;
			if (m_dataType == 2) {
				onLevel({});
			}
		}
		return;
	}
	if (!m_inData) {
		return;
	}
	m_dataBuffer += chunk.bytes;
	if (m_dataBuffer.size() >= m_dataSize) {
		m_dataBuffer.resize(m_dataSize);
		m_dataBlocks[m_dataType] = m_dataBuffer;
		m_inData = false;
		if (m_dataType == 2) {
			onLevel(m_dataBuffer);
		}
	}
}

void RemoteWorld::applyStringBlock(const DataBlockChunk& chunk)
{
	// 0x00643060 starts a block of `size` bytes, 0x006430c0 fills it.
	if (chunk.start) {
		m_inString = true;
		m_stringSize = chunk.size;
		m_stringBuffer.clear();
		return;
	}
	if (!m_inString) {
		return;
	}
	m_stringBuffer += chunk.bytes;
	if (m_stringBuffer.size() >= m_stringSize) {
		m_stringBuffer.resize(m_stringSize);
		m_inString = false;
		ChatMessage message;
		if (decodeChat(m_stringBuffer, message)) {
			m_chat.push_back({message.player, message.channel, message.source, message.text});
		}
	}
}

void RemoteWorld::applyPythonCommand(const PythonCommandInfo& info)
{
	// 0x00511ba0: type 100 messages are kills, arguments (kind, victim,
	// attacker, ...); kind 2 is told to the victim, 0 and 1 to the attacker.
	if (info.type != 100 || info.args.size() < 3) {
		return;
	}
	KillReport report;
	report.victim = info.args[1];
	report.attacker = info.args[2];
	m_kills.push_back(report);
	if (auto it = m_players.find(report.victim); it != m_players.end()) {
		++it->second.deaths;
		it->second.alive = false;
	}
	if (report.attacker != report.victim) {
		if (auto it = m_players.find(report.attacker); it != m_players.end()) {
			++it->second.kills;
		}
	}
}

bool RemoteWorld::apply(const GameEvent& event)
{
	switch (event.type) {
	case kEvCreatePlayer: {
		CreatePlayerInfo info;
		if (!parseCreatePlayer(event, info)) {
			return false;
		}
		RemotePlayer& p = playerSlot(info.id);
		p.name = info.name;
		p.team = info.team;
		p.local = !info.remote;
		p.ai = info.remote && info.ai;
		p.profileId = info.profileId;
		if (info.object != 0) {
			p.networkableGhost = info.object; // 0x0050a8c0 registers the player's networkable under it
			m_playerGhosts[info.object] = info.id;
		}
		if (p.local) {
			m_localPlayer = info.id;
		}
		return true;
	}
	case kEvDestroyPlayer:
		if (auto it = m_players.find(event.i(0)); it != m_players.end()) {
			m_playerGhosts.erase(it->second.networkableGhost);
		}
		m_players.erase(event.i(0));
		if (event.i(0) == m_localPlayer) {
			m_localPlayer = -1;
		}
		return true;
	case kEvChangePlayerName:
		if (auto it = m_players.find(event.i(0)); it != m_players.end()) {
			const std::string& name = event.text(0);
			it->second.name = name.substr(0, name.find('\0'));
		}
		return true;
	case kEvCreateObject: {
		CreateObjectInfo info;
		if (!parseCreateObject(event, info)) {
			return false;
		}
		// 0x0050b450: an object of `count` ghosts, one per PlayerControlObject
		// of its tree (0x0067c690), numbered from `ghost`.
		const std::vector<GhostPartLayout>* layout = ghostLayout(info.templateId);
		const auto shared = layout ? m_layouts[info.templateId] : nullptr;
		std::vector<uint16_t> seats;
		for (uint32_t n = 0; n < info.count; ++n) {
			const uint16_t ghost = static_cast<uint16_t>(info.ghost + n);
			RemoteObject& o = m_objects[ghost];
			o = {};
			o.ghost = ghost;
			o.rootGhost = info.ghost;
			o.part = static_cast<int>(n);
			o.templateId = info.templateId;
			o.team = info.team;
			o.hasPosition = info.hasPosition;
			o.position = info.position;
			o.rotation = info.rotation;
			m_objectHints.erase(ghost);
			m_ghostParts.erase(ghost);
			if (layout && n < layout->size()) {
				const GhostPartLayout& part = (*layout)[n];
				o.templateId = part.templateId;
				o.kind = part.groups.empty() ? NetworkableKind::Unknown : part.groups.front().hints.kind;
				for (const GhostNodeLayout& node : part.nodes) {
					RemotePart p;
					p.name = node.name;
					p.templateId = node.templateId;
					p.parent = node.parent;
					p.kind = node.kind;
					p.group = node.group;
					o.parts.push_back(std::move(p));
				}
				m_ghostParts[ghost] = {shared, n};
			} else if (m_hints && !layout) {
				NetworkableHints hints = m_hints(info.templateId);
				o.kind = hints.kind;
				m_objectHints[ghost] = hints;
			}
			o.templateName = m_resolver ? m_resolver(o.templateId) : std::string();
			if (n > 0) {
				seats.push_back(ghost);
			}
		}
		if (info.count > 0) {
			m_objects[info.ghost].seats = seats;
		}
		return true;
	}
	case kEvCreateKit: {
		CreateKitInfo info;
		if (!parseCreateKit(event, info)) {
			return false;
		}
		RemoteObject& o = m_objects[info.ghost];
		o = {};
		o.ghost = info.ghost;
		o.templateId = info.templateId;
		o.templateName = m_resolver ? m_resolver(info.templateId) : std::string();
		o.kit = true;
		o.hasPosition = true;
		o.position = info.position;
		o.kind = NetworkableKind::Kit;
		NetworkableHints kit;
		kit.kind = NetworkableKind::Kit;
		m_objectHints[o.ghost] = kit;
		return true;
	}
	case kEvDestroyObject: {
		// The root's ghost takes its seats' with it.
		const uint16_t ghost = static_cast<uint16_t>(event.u(0));
		std::vector<uint16_t> gone {ghost};
		if (auto it = m_objects.find(ghost); it != m_objects.end() && it->second.rootGhost == ghost) {
			gone.insert(gone.end(), it->second.seats.begin(), it->second.seats.end());
		}
		for (uint16_t g : gone) {
			m_objects.erase(g);
			m_hidden.erase(g);
			m_objectHints.erase(g);
			m_ghostParts.erase(g);
		}
		return true;
	}
	case kEvEnterVehicle: // 0x0050b850: the player, the ghost of the seat (a PlayerControlObject) taken
		if (auto it = m_players.find(event.i(0)); it != m_players.end()) {
			if (auto old = m_objects.find(it->second.vehicleGhost); old != m_objects.end() && old->second.occupant == event.i(0)) {
				old->second.occupant = -1;
			}
			it->second.vehicleGhost = static_cast<uint16_t>(event.u(1));
		}
		if (auto it = m_objects.find(static_cast<uint16_t>(event.u(1))); it != m_objects.end()) {
			it->second.occupant = event.i(0);
		}
		return true;
	case kEvExitVehicle:
		if (auto it = m_players.find(event.i(0)); it != m_players.end()) {
			if (auto seat = m_objects.find(it->second.vehicleGhost); seat != m_objects.end() && seat->second.occupant == event.i(0)) {
				seat->second.occupant = -1;
				seat->second.hasOccupantState = false;
			}
			it->second.vehicleGhost = 0;
		}
		return true;
	case kEvHandlePickup: // 0x0050bfd0: the game logic's slot 60 (the kit, the player, the soldier)
	case kEvHandleDrop:   // 0x0050c1c0: slot 61, then the kit moves to the position
		// (player 8, kit 16, soldier 16): the soldier is the one asked for
		// interface 0xc49d (0x004bb350; the soldier's QueryInterface
		// 0x00585af0 answers it at +0x188), the kit's template is checked
		// for Kit (0x9496).
		if (auto it = m_players.find(event.i(0)); it != m_players.end()) {
			it->second.soldierGhost = static_cast<uint16_t>(event.u(2));
			it->second.kitGhost = event.type == kEvHandlePickup ? static_cast<uint16_t>(event.u(1)) : 0;
			if (event.type == kEvHandlePickup) {
				it->second.alive = true;
			}
		}
		if (event.type == kEvHandleDrop) {
			if (auto it = m_objects.find(static_cast<uint16_t>(event.u(1))); it != m_objects.end()) {
				it->second.position = event.vec(3);
				it->second.hasPosition = true;
			}
		}
		return true;
	case kEvDataBlock: {
		DataBlockChunk chunk;
		if (parseDataBlock(event, chunk)) {
			applyDataBlock(chunk);
		}
		return true;
	}
	case kEvStringBlock: {
		DataBlockChunk chunk;
		if (parseStringBlock(event, chunk)) {
			applyStringBlock(chunk);
		}
		return true;
	}
	case kEvPostRemote: {
		PostRemoteInfo info;
		if (!parsePostRemote(event, info)) {
			return false;
		}
		if (info.category == kMsgGameCategory && info.id == 1) {
			m_gameState = static_cast<uint32_t>(info.payloadInt());
		}
		if (info.category == kMsgNetworkCategory && info.id == kNePlayerSpawned && m_localPlayer >= 0) {
			m_players[m_localPlayer].alive = true;
		}
		if (info.category == kMsgNetworkCategory && info.id == kNePlayerDead && m_localPlayer >= 0) {
			m_players[m_localPlayer].alive = false;
		}
		m_messages.push_back(info);
		return true;
	}
	case kEvKilledBy: {
		KilledByInfo info;
		if (!parseKilledBy(event, info)) {
			return false;
		}
		KillReport report;
		report.victim = info.victim;
		report.attacker = info.hasAttacker ? info.attacker : -1;
		report.weapon = info.armorValue;
		report.from = info.position;
		m_kills.push_back(report);
		return true;
	}
	case kEvPythonCommand: {
		PythonCommandInfo info;
		if (parsePythonCommand(event, info)) {
			applyPythonCommand(info);
		}
		return true;
	}
	case kEvRank: // 0x0050ce10: kind 1 sets the player's rank (vtable 0x1a8)
		if (auto it = m_players.find(event.i(3)); event.u(0) == 1 && it != m_players.end()) {
			it->second.rank = event.i(1) - 1;
		}
		return true;
	case kEvBeginRound:
		m_round.state = event.u(0);
		m_round.endTick = event.u(1);
		if (event.i(2) >= 0) {
			m_round.playersNeeded = event.i(2);
		}
		m_roundStarted = true;
		m_roundEnded = false;
		return true;
	case kEvEndOfRound:
		m_endOfRound.roundsLeft = event.u(0);
		m_endOfRound.winner = event.u(1);
		m_endOfRound.victoryType = event.u(2);
		m_roundStarted = false;
		m_roundEnded = true;
		return true;
	case kEvKickBan: // 0x0050cec0; the client's handler (0x0050cfd0) posts 2/0x4d for the HUD
		m_kicks.push_back({static_cast<int>(event.u(0)), event.u(1) != 0, event.u(2)});
		return true;
	default:
		m_unhandled.push_back(event);
		return false;
	}
}

void RemoteWorld::apply(const GhostBlock& ghosts)
{
	if (!ghosts.present) {
		return;
	}
	m_tick = ghosts.tick;
	m_controlInLastBlock = ghosts.control.has_value();
	if (!ghosts.control) {
		m_controlStateValid = false;
	}
	if (ghosts.control) {
		m_hasControl = true;
		m_control = *ghosts.control;
		++m_controlRecords;
		if (auto it = m_objects.find(m_control.ghost); it != m_objects.end()) {
			it->second.position = m_control.position;
			it->second.hasPosition = true;
		}
		decodeControl();
		if (m_localPlayer >= 0) {
			RemotePlayer& p = m_players[m_localPlayer];
			if (p.soldierGhost == 0) {
				p.soldierGhost = m_control.ghost;
			}
			p.alive = true;
		}
	}
	// Delta-coded positions are relative to the control-object record's
	// position in this block (0x006874f0), else to the origin (0x00687140).
	const Vec3f reference = ghosts.control ? ghosts.control->position : Vec3f {};
	for (const GhostUpdate& u : ghosts.updates) {
		if (u.kind == 3) {
			// Out of the client's scope: hidden, kept for its return.
			if (auto it = m_objects.find(u.ghost); it != m_objects.end()) {
				m_hidden[u.ghost] = std::move(it->second);
				m_objects.erase(it);
			}
			continue;
		}
		if (u.kind != 1) {
			continue;
		}
		if (auto it = m_hidden.find(u.ghost); it != m_hidden.end()) {
			m_objects[u.ghost] = std::move(it->second);
			m_hidden.erase(it);
		}
		if (auto pg = m_playerGhosts.find(u.ghost); pg != m_playerGhosts.end()) {
			BitReader in(u.state.data(), u.state.size());
			NetworkableState state;
			NetworkableHints hints;
			hints.kind = NetworkableKind::Player;
			if (decodeNetworkable(in, hints, reference, state) && state.complete) {
				RemotePlayer& p = m_players[pg->second];
				const PlayerNetState& n = state.player;
				if (n.hasStats) {
					p.score = n.score;
					p.kills = n.kills;
					p.deaths = n.deaths;
					p.teamwork = n.teamwork;
					p.teamKills = n.teamKills;
				}
				if (n.ping >= 0) {
					p.ping = n.ping;
				}
				if (n.team >= 0) {
					p.team = n.team;
				}
				if (n.hasAlive) {
					p.alive = n.alive;
				}
				if (n.squad >= 0) {
					p.squad = n.squad;
				}
				p.squadLeader = n.squadLeader;
				if (n.hasCommander) {
					p.commander = n.commander;
				}
				if (n.kit >= 0) {
					p.kit = n.kit;
				}
				if (n.rank >= 0) {
					p.rank = n.rank;
				}
			}
			continue;
		}
		RemoteObject& o = m_objects[u.ghost];
		o.ghost = u.ghost;
		++o.updates;
		o.lastTick = ghosts.tick;
		o.state = u.state;
		o.stateBits = u.stateBits;
		if (const auto part = m_ghostParts.find(u.ghost); part != m_ghostParts.end() && part->second.first) {
			const GhostPartLayout& layout = (*part->second.first)[part->second.second];
			BitReader in(u.state.data(), (u.stateBits + 7) / 8);
			std::vector<NetworkableState> states;
			o.decodedAll = decodeGhostState(in, layout, reference, states);
			o.decoded = !states.empty() || layout.groups.empty();
			if (!states.empty()) {
				applyState(o, states.front());
				o.decoded = states.front().complete;
			}
			applyParts(o, layout, states);
			continue;
		}
		const auto hint = m_objectHints.find(u.ghost);
		if (hint == m_objectHints.end() || hint->second.kind == NetworkableKind::Unknown) {
			continue;
		}
		BitReader in(u.state.data(), (u.stateBits + 7) / 8);
		NetworkableState state;
		if (decodeNetworkable(in, hint->second, reference, state)) {
			applyState(o, state);
		}
	}
}

void RemoteWorld::decodeControl()
{
	// 0x00580cd0: only with the record's ghost (0x00678d70) is the rest
	// read; without it the 12-bit length skips it.
	m_controlStateValid = false;
	const auto part = m_ghostParts.find(m_control.ghost);
	if (part == m_ghostParts.end() || !part->second.first || m_control.restBits == 0) {
		return;
	}
	const GhostPartLayout& layout = (*part->second.first)[part->second.second];
	const GhostPartLayout* second = nullptr;
	if (m_control.second) {
		const auto other = m_ghostParts.find(m_control.second);
		if (other == m_ghostParts.end() || !other->second.first) {
			return;
		}
		second = &(*other->second.first)[other->second.second];
	}
	BitReader in(m_control.rest.data(), m_control.rest.size());
	ControlState state;
	PlayerOwnStateHints hints;
	if (!decodeControlState(in, layout, second, m_control.position, hints, state, &m_historySlots)
		|| in.position() > m_control.restBits) {
		return;
	}
	// A fresh state goes into its history slot (0x00580800 -> 0x0057f750),
	// for the relative records written against it.
	if (!state.recorded) {
		m_historySlots[static_cast<size_t>(state.slot) & 7] = motionHistoryOf(state);
	}
	if (state.recorded) {
		++m_relativeStates;
	}
	m_controlState = std::move(state);
	m_controlStateValid = true;
	++m_controlStates;
	// The state also brings the ghost's object up to date (as the mode-3
	// reader applies it, 0x0057f720 with 1). A soldier's mode-3 pitch and
	// roll are its control's twist, not angles: left out here.
	const auto applyTo = [this](uint16_t ghost, const GhostPartLayout& l, std::vector<NetworkableState> states) {
		const auto it = m_objects.find(ghost);
		if (it == m_objects.end() || states.empty()) {
			return;
		}
		if (states.front().kind == NetworkableKind::Soldier) {
			states.front().mask &= ~0xcu;
		}
		applyState(it->second, states.front());
		applyParts(it->second, l, states);
	};
	applyTo(m_control.ghost, layout, m_controlState.groups);
	if (second) {
		applyTo(m_control.second, *second, m_controlState.second);
	}
}

const std::vector<GhostPartLayout>* RemoteWorld::ghostLayout(uint32_t templateId)
{
	if (!m_ghostTemplates) {
		return nullptr;
	}
	auto it = m_layouts.find(templateId);
	if (it == m_layouts.end()) {
		const GhostTemplateNode root = m_ghostTemplates(templateId);
		std::shared_ptr<const std::vector<GhostPartLayout>> layout;
		if (root.templateId != 0 || !root.name.empty()) {
			layout = std::make_shared<const std::vector<GhostPartLayout>>(buildGhostLayout(root));
		}
		it = m_layouts.emplace(templateId, layout).first;
	}
	return it->second.get();
}

const RemotePart* RemoteObject::findPart(const std::string& name) const
{
	for (const RemotePart& p : parts) {
		if (p.name.size() == name.size() && std::equal(p.name.begin(), p.name.end(), name.begin(), [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			})) {
			return &p;
		}
	}
	return nullptr;
}

namespace {

// An update carries what changed (its mask); the rest stays as it was.
void mergePart(RemotePart& p, const NetworkableState& s)
{
	p.last = s;
	switch (s.kind) {
	case NetworkableKind::RotationalBundle:
		for (int i = 0; i < 3; ++i) {
			if (s.mask & (1u << i)) {
				p.rotation[i] = s.rotation[i];
				p.hasRotation = true;
			}
			if (s.mask & (8u << i)) {
				p.rotationSpeed[i] = s.rotationSpeed[i];
			}
		}
		break;
	case NetworkableKind::Spring:
		if (s.springPresent) {
			p.hasSpring = true;
			p.springOffset = s.springOffset;
			p.springVelocity = s.springVelocity;
		}
		break;
	case NetworkableKind::Engine:
		if (s.mask & 2) {
			p.gear = s.gear;
		}
		if (s.mask & 4) {
			p.engineRotation[1] = s.engineRotation[1];
		}
		if (s.mask & 8) {
			p.engineRotation[0] = s.engineRotation[0];
		}
		for (int i = 0; i < 3; ++i) {
			if (s.mask & (0x20u << i)) {
				p.engineInputs[i] = s.engineInputs[i];
			}
		}
		if (s.mask & 0x400) {
			p.rpm = s.rpm;
		}
		break;
	default:
		break;
	}
	if (s.hasPosition) {
		p.hasPosition = true;
		p.position = s.position;
	}
	if (s.hasOrientation) {
		p.hasOrientation = true;
		p.orientation = s.orientation;
	}
	if (s.hasHealth) {
		p.health = s.health;
	}
}

} // namespace

void RemoteWorld::applyParts(RemoteObject& o, const GhostPartLayout& layout, const std::vector<NetworkableState>& states)
{
	if (o.parts.size() != layout.nodes.size()) {
		return;
	}
	for (size_t i = 0; i < layout.nodes.size(); ++i) {
		const int g = layout.nodes[i].group;
		if (g >= 0 && static_cast<size_t>(g) < states.size()) {
			mergePart(o.parts[i], states[static_cast<size_t>(g)]);
		}
	}
	// The ghost root's own networkable: a seat's weapons or occupant.
	if (states.empty()) {
		return;
	}
	const NetworkableState& root = states.front();
	if (root.kind == NetworkableKind::PlayerControlObject) {
		if (root.occupantBits == 0 && !root.occupant) {
			for (int g = 0; g < 3; ++g) {
				if (root.mask & (1u << g)) {
					o.seatWeapon[g] = root.seatWeapon[g];
				}
			}
			if (root.mask & 8) {
				o.seatDucking = root.seatDucking;
			}
		}
		if (root.occupant) {
			o.hasOccupantState = true;
			o.occupantState = *root.occupant;
		}
	}
	if (root.kind == NetworkableKind::SimpleObject) {
		const uint32_t slots[3] = {0x2000, 0x4000, 0x80000};
		for (int g = 0; g < 3; ++g) {
			if (root.mask & slots[g]) {
				o.seatWeapon[g] = root.seatWeapon[g];
			}
		}
	}
}

void RemoteWorld::applyState(RemoteObject& o, const NetworkableState& s)
{
	o.decoded = s.complete;
	if ((s.mask & 0x10000) && s.kind == NetworkableKind::Soldier && !s.ragdollParticles.empty()) {
		o.ragdoll = true;
		o.ragdollParticles = s.ragdollParticles;
		++o.ragdollUpdates;
	} else if (s.kind == NetworkableKind::Soldier && s.hasHealth && s.health > 0) {
		o.ragdoll = false; // alive again (revived): the body is gone
		o.ragdollParticles.clear();
	}
	if (s.hasPosition) {
		o.hasPosition = true;
		o.position = s.position;
	}
	if (s.hasOrientation) {
		o.hasOrientation = true;
		o.orientation = s.orientation;
	}
	if (s.hasVelocity) {
		o.hasVelocity = true;
		o.velocity = s.velocity;
	}
	if (s.hasAngles) {
		o.hasAngles = true;
		if (s.mask & 2) {
			o.yaw = s.yaw;
		}
		if (s.mask & 4) {
			o.pitch = s.pitch;
		}
		if (s.mask & 0x10) {
			o.aimPitch = s.aimPitch;
		}
	}
	if (s.weaponZoom >= 0) {
		o.weaponZoom = s.weaponZoom;
	}
	if (s.kind == NetworkableKind::Soldier && (s.mask & 0x2000)) {
		o.weapon = s.weapon;
	}
	if (s.hasHealth) {
		o.health = s.health;
	}
	if (s.kind == NetworkableKind::ControlPoint) {
		if (s.mask & 1) {
			o.team = s.team;
		}
		if (s.mask & 4) {
			o.captureProgress = s.cpValues[0];
			o.captureRate = s.cpValues[1];
		}
		if (s.mask & 0x20) {
			o.raisedFlag = s.cpValue;
		}
	}
}

std::vector<RemoteObject> RemoteWorld::objectList() const
{
	std::vector<RemoteObject> list;
	list.reserve(m_objects.size());
	for (const auto& [ghost, o] : m_objects) {
		list.push_back(o);
	}
	return list;
}

const RemoteObject* RemoteWorld::object(uint16_t ghost) const
{
	const auto it = m_objects.find(ghost);
	return it == m_objects.end() ? nullptr : &it->second;
}

bool RemoteWorld::partPose(uint16_t rootGhost, const std::string& part, Vec3f& offset, Vec3f& rotation) const
{
	const RemoteObject* root = object(rootGhost);
	if (!root) {
		return false;
	}
	std::vector<const RemoteObject*> ghosts {root};
	for (const uint16_t seat : root->seats) {
		if (const RemoteObject* o = object(seat)) {
			ghosts.push_back(o);
		}
	}
	const auto same = [](const std::string& a, const std::string& b) {
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	};
	for (const RemoteObject* o : ghosts) {
		for (size_t i = 1; i < o->parts.size(); ++i) {
			const RemotePart& p = o->parts[i];
			if (!same(p.name, part)) {
				continue;
			}
			if (p.hasRotation) {
				offset = {};
				rotation = {p.rotation[0], p.rotation[1], p.rotation[2]};
				return true;
			}
			if (p.hasSpring) {
				offset = p.springOffset;
				rotation = {};
				return true;
			}
		}
	}
	return false;
}

std::vector<RemotePlayer> RemoteWorld::playerList() const
{
	std::vector<RemotePlayer> list;
	for (const auto& [id, p] : m_players) {
		list.push_back(p);
	}
	return list;
}

const RemotePlayer* RemoteWorld::player(int id) const
{
	const auto it = m_players.find(id);
	return it == m_players.end() ? nullptr : &it->second;
}

std::vector<std::string> RemoteWorld::dataBlockStrings(uint32_t type) const
{
	std::vector<std::string> strings;
	const auto it = m_dataBlocks.find(type);
	if (it == m_dataBlocks.end()) {
		return strings;
	}
	std::string current;
	for (char c : it->second) {
		if (c >= 0x20 && c < 0x7f) {
			current.push_back(c);
			continue;
		}
		if (current.size() >= 3) {
			strings.push_back(current);
		}
		current.clear();
	}
	if (current.size() >= 3) {
		strings.push_back(current);
	}
	return strings;
}

namespace {

// 0x005155b0: a u16 length and the bytes.
bool readString(BitReader& in, std::string& out)
{
	const uint32_t length = in.read(16);
	if (in.bad() || in.remaining() < static_cast<size_t>(length) * 8) {
		return false;
	}
	out.clear();
	for (uint32_t i = 0; i < length; ++i) {
		out.push_back(static_cast<char>(in.read(8)));
	}
	return true;
}

void writeString(BitWriter& out, const std::string& text)
{
	out.write(static_cast<uint32_t>(text.size()), 16);
	for (char c : text) {
		out.write(static_cast<uint8_t>(c), 8);
	}
}

std::string bytesOf(const BitWriter& out)
{
	const std::vector<uint8_t>& data = out.data();
	return std::string(data.begin(), data.end());
}

BitReader readerOf(const std::string& data)
{
	return BitReader(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

} // namespace

bool RemoteWorld::decodeEndOfRoundData(const std::string& block, EndOfRoundData& out)
{
	// 0x0059c740.
	out = {};
	BitReader in = readerOf(block);
	const uint32_t size = in.read(32);
	const uint32_t compressedSize = in.read(32);
	if (in.bad() || compressedSize > 0x8000 || in.remaining() < static_cast<size_t>(compressedSize) * 8) {
		return false;
	}
	std::vector<uint8_t> compressed(compressedSize);
	for (uint8_t& b : compressed) {
		b = static_cast<uint8_t>(in.read(8));
	}
	out.text.resize(size);
	uLongf length = size;
	if (size > 0
		&& (uncompress(reinterpret_cast<Bytef*>(out.text.data()), &length, compressed.data(), compressedSize) != Z_OK
			|| length != size)) {
		return false;
	}
	out.endOfRoundDelay = in.readSigned(32);
	const uint32_t count = in.read(8);
	for (uint32_t i = 0; i < count && !in.bad(); ++i) {
		EndOfRoundData::Player p;
		p.id = static_cast<int>(in.read(8));
		p.kills = in.readSigned(16);
		p.skillScore = in.readSigned(16);
		p.deaths = in.readSigned(16);
		p.score = in.readSigned(16);
		p.teamworkScore = in.readSigned(16);
		p.commanderScore = in.readSigned(16);
		out.players.push_back(p);
	}
	return !in.bad();
}

std::string RemoteWorld::encodeEndOfRoundData(const EndOfRoundData& data)
{
	// 0x0059cbc0.
	uLongf compressedSize = compressBound(static_cast<uLong>(data.text.size()));
	std::vector<uint8_t> compressed(compressedSize);
	compress2(compressed.data(), &compressedSize, reinterpret_cast<const Bytef*>(data.text.data()),
		static_cast<uLong>(data.text.size()), Z_DEFAULT_COMPRESSION);
	BitWriter out;
	out.write(static_cast<uint32_t>(data.text.size()), 32);
	out.write(static_cast<uint32_t>(compressedSize), 32);
	out.writeBytes(compressed.data(), compressedSize);
	out.writeSigned(data.endOfRoundDelay, 32);
	out.write(static_cast<uint32_t>(data.players.size()), 8);
	for (const EndOfRoundData::Player& p : data.players) {
		out.write(static_cast<uint32_t>(p.id), 8);
		for (int v : {p.kills, p.skillScore, p.deaths, p.score, p.teamworkScore, p.commanderScore}) {
			out.writeSigned(v, 16);
		}
	}
	return bytesOf(out);
}

bool RemoteWorld::endOfRoundData(EndOfRoundData& out) const
{
	const auto it = m_dataBlocks.find(8);
	return it != m_dataBlocks.end() && decodeEndOfRoundData(it->second, out);
}

std::vector<RemoteWorld::MapEntry> RemoteWorld::mapList() const
{
	std::vector<MapEntry> maps;
	const auto it = m_dataBlocks.find(5);
	if (it == m_dataBlocks.end()) {
		return maps;
	}
	BitReader in = readerOf(it->second);
	const uint32_t count = in.read(16);
	in.read(16); // the current map
	for (uint32_t i = 0; i < count && !in.bad(); ++i) {
		MapEntry entry;
		if (!readString(in, entry.name) || !readString(in, entry.gameMode)) {
			break;
		}
		entry.size = in.read(16);
		if (in.bad()) {
			break;
		}
		maps.push_back(entry);
	}
	return maps;
}

int RemoteWorld::currentMapIndex() const
{
	const auto it = m_dataBlocks.find(5);
	if (it == m_dataBlocks.end() || it->second.size() < 4) {
		return -1;
	}
	BitReader in = readerOf(it->second);
	in.read(16);
	return static_cast<int>(in.read(16));
}

std::string RemoteWorld::encodeMapList(const std::vector<MapEntry>& maps, int current)
{
	BitWriter out;
	out.write(static_cast<uint32_t>(maps.size()), 16);
	out.write(static_cast<uint32_t>(std::max(0, current)), 16);
	for (const MapEntry& m : maps) {
		writeString(out, m.name);
		writeString(out, m.gameMode);
		out.write(m.size, 16);
	}
	return bytesOf(out);
}

bool RemoteWorld::decodeSettings(const std::string& data, ServerSettings& s)
{
	s = {};
	BitReader in = readerOf(data);
	if (!readString(in, s.name)) {
		return false;
	}
	const uint32_t pieces = in.read(16);
	for (uint32_t i = 0; i < pieces && i < 16; ++i) {
		std::string piece;
		if (!readString(in, piece)) {
			return false;
		}
		s.welcome.push_back(piece);
	}
	bool* flags[7] = {&s.initial, &s.voipEnabled, &s.allowNoseCam, &s.hitIndicator, &s.tkPunishByDefault, &s.allowFreeCam,
		&s.allowExternalViews};
	for (bool* f : flags) {
		*f = in.read(1) == 1;
	}
	s.voipServerAddress = in.read(32);
	s.voipServerPort = in.read(16);
	s.voipClientPort = in.read(16);
	s.voipQuality = in.readSigned(32);
	s.gameSpyPort = in.read(16);
	s.gamePort = in.read(16);
	s.radioSpamInterval = in.readSigned(32);
	s.radioMaxSpamFlagCount = in.readSigned(32);
	s.radioBlockedDurationTime = in.readSigned(32);
	s.contentCheck = in.read(1) == 1;
	s.votingEnabled = in.read(1) == 1;
	s.playersNeededToStart = in.read(16);
	s.friendlyFireWithMines = in.read(1) == 1;
	s.allowTitanMovement = in.read(1) == 1;
	s.complete = !in.bad();
	return true;
}

std::string RemoteWorld::encodeSettings(const ServerSettings& s)
{
	BitWriter out;
	writeString(out, s.name);
	out.write(static_cast<uint32_t>(s.welcome.size()), 16);
	for (const std::string& piece : s.welcome) {
		writeString(out, piece);
	}
	for (bool f : {s.initial, s.voipEnabled, s.allowNoseCam, s.hitIndicator, s.tkPunishByDefault, s.allowFreeCam,
			 s.allowExternalViews}) {
		out.write(f ? 1 : 0, 1);
	}
	out.write(s.voipServerAddress, 32);
	out.write(s.voipServerPort, 16);
	out.write(s.voipClientPort, 16);
	out.writeSigned(s.voipQuality, 32);
	out.write(s.gameSpyPort, 16);
	out.write(s.gamePort, 16);
	out.writeSigned(s.radioSpamInterval, 32);
	out.writeSigned(s.radioMaxSpamFlagCount, 32);
	out.writeSigned(s.radioBlockedDurationTime, 32);
	out.write(s.contentCheck ? 1 : 0, 1);
	out.write(s.votingEnabled ? 1 : 0, 1);
	out.write(s.playersNeededToStart, 16);
	out.write(s.friendlyFireWithMines ? 1 : 0, 1);
	out.write(s.allowTitanMovement ? 1 : 0, 1);
	return bytesOf(out);
}

RemoteWorld::ServerSettings RemoteWorld::settings() const
{
	ServerSettings s;
	if (const auto it = m_dataBlocks.find(0); it != m_dataBlocks.end()) {
		decodeSettings(it->second, s);
	}
	return s;
}

std::string RemoteWorld::serverName() const
{
	const auto it = m_dataBlocks.find(0);
	if (it == m_dataBlocks.end()) {
		return {};
	}
	BitReader in = readerOf(it->second);
	std::string name;
	return readString(in, name) ? name : std::string();
}

bool RemoteWorld::decodeLevel(const std::string& data, LevelInfo& level)
{
	level = {};
	BitReader in = readerOf(data);
	if (!readString(in, level.gameMode) || !readString(in, level.levelDirectory) || !readString(in, level.mapName)) {
		return false;
	}
	level.maxPlayers = in.readSigned(32);
	level.commander = in.read(1) == 1;
	level.seed = in.readSigned(32);
	return !in.bad();
}

std::string RemoteWorld::encodeLevel(const LevelInfo& level)
{
	BitWriter out;
	writeString(out, level.gameMode);
	writeString(out, level.levelDirectory);
	writeString(out, level.mapName);
	out.writeSigned(level.maxPlayers, 32);
	out.write(level.commander ? 1 : 0, 1);
	out.writeSigned(level.seed, 32);
	return bytesOf(out);
}

void RemoteWorld::onLevel(const std::string& data)
{
	LevelInfo level;
	decodeLevel(data, level);
	if (m_levels > 0) {
		// A map change: the old level's objects go (the server makes the
		// new level's), and their templates with it.
		m_objects.clear();
		m_hidden.clear();
		m_objectHints.clear();
		m_ghostParts.clear();
		m_layouts.clear();
		m_hasControl = false;
		m_roundStarted = false;
		m_roundEnded = false;
		for (auto& [id, p] : m_players) {
			p.alive = false;
			p.soldierGhost = p.vehicleGhost = p.kitGhost = 0;
		}
		m_controlInLastBlock = m_controlStateValid = false;
	}
	++m_levels;
	m_level = level;
	m_levelChanges.push_back(level);
}

} // namespace dice::hfe::net
