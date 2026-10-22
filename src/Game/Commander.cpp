// The commander: one per team, its tools and its orders to squads
// (docs/engine.md, "Commander"). GameWorld's members kept apart from
// GameWorld.cpp.
//
// In BF2142.exe the team's commander lives in the SquadManager
// (dice.bf.SquadManager, vtable 0x00929d40: slot 0x74 sets it, 0x78 reads
// it, 0x64 pops the next applicant), the tools' assets are gathered per
// team and type by 0x005a1440 and fired by 0x005a1270 / 0x005a0e50, the UAV
// and the satellite scan run on the gameLogic's timers (0x005a23b0,
// 0x005a2450), and the supply drop is 0x0043d6d0.
#include "GameWorld.h"

#include "Physics/MaterialManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

int integer(const world::ObjectTemplate& t, const char* name, int fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : std::atoi(it->second.c_str());
}

bool sameName(const std::string& a, const std::string& b)
{
	const world::CaseInsensitiveLess less;
	return !less(a, b) && !less(b, a);
}

// rcType (0x004daf74's names; the property also takes the number):
// RCArtillery 1, RCSatellite 2, RCUav 3, RCOrbitalStrike 4. The others
// (turning, titans, consoles) are no commander's tools.
bool abilityOfRcType(const std::string& type, GameWorld::CommanderAbility& ability)
{
	using A = GameWorld::CommanderAbility;
	if (sameName(type, "RCArtillery") || type == "1") {
		ability = A::Artillery;
	} else if (sameName(type, "RCSatellite") || type == "2") {
		ability = A::Satellite;
	} else if (sameName(type, "RCUav") || type == "3") {
		ability = A::Uav;
	} else if (sameName(type, "RCOrbitalStrike") || type == "4") {
		ability = A::OrbitalStrike;
	} else {
		return false;
	}
	return true;
}

} // namespace

// sh_setEnableCommander posts event 0x47 (0x00568640); the game modes
// enable it, squad deathmatch (gpm_sl) doesn't. Disabled, the teams lose
// their commanders.
void GameWorld::setCommanderEnabled(bool enabled)
{
	if (!enabled) {
		for (int team = 1; team <= 2; ++team) {
			setCommander(team, -1);
			m_commanders[team].applicants.clear();
		}
	}
	m_commanderEnabled = enabled;
}

int GameWorld::commander(int team) const
{
	return team == 1 || team == 2 ? m_commanders[team].commander : -1;
}

// 0x004c8120: the old commander's flag goes (player slot 0x124), the new
// one leaves its squad (0x004c7bb0) and takes it, the clients hear of it
// (event 0x13 kind 3), the applicants are dropped, and the scripts get
// ChangedCommander (event 10: team, old, new).
bool GameWorld::setCommander(int team, int player)
{
	if (team != 1 && team != 2) {
		return false;
	}
	if (player != -1) {
		const Player* p = findPlayer(player);
		if (!m_commanderEnabled || !p || p->team != team) {
			return false;
		}
	}
	CommanderTeam& side = m_commanders[team];
	const int old = side.commander;
	if (old == player) {
		return true;
	}
	side.commander = player;
	if (player != -1) {
		joinSquad(player, 0);
		// The other applicants hear HUD_COMMANDERAPPLY_POSFILLED (PostRemote
		// 0x22) and the list is cleared; nobody is offered command.
		side.applicants.clear();
		side.candidate = -1;
		side.pickAt = -1;
	}
	raise("ChangedCommander", {ScriptValue::integer(team), ScriptValue::player(old), ScriptValue::player(player)});
	return true;
}

// The SquadManager's election timings (its constructor 0x004c96c0):
// applications wait 15 s (+0x14) before the best applicant is offered
// command; after a resignation the next is offered it 10 s later (+0x18).
namespace {
constexpr double kApplyPeriod = 15;
constexpr double kAfterResign = 10;
} // namespace

void GameWorld::setPlayerRank(int player, int rank)
{
	if (Player* p = findPlayer(player)) {
		p->rank = rank;
	}
}

// Commander.commanderApply (HudManager slot 0x31c, 0x007d7050; the
// server's Commander event kind 4, SquadManager slot 0x60 → 0x004c7160):
// the player joins the team's applicants. The first application while
// nobody is being offered command starts the apply period (event 7 after
// SquadManager +0x14, 15 s; HUD_COMMANDERAPPLY_TIMETOSTART counts down to
// the round's start meanwhile). Not modelled: the client refusing an
// applicant still barred after a mutiny (player slots 0x140, 0x148,
// HUD_HUD_COMMANDER_CANNOT_APPLY_FOR_MINUTES); there is no mutiny here.
// Bots don't apply: AIDLL never calls it (its commander is the team's
// AI, BotManager::command).
void GameWorld::applyForCommander(int player)
{
	const Player* p = findPlayer(player);
	if (!m_commanderEnabled || !p || (p->team != 1 && p->team != 2)) {
		return;
	}
	CommanderTeam& side = m_commanders[p->team];
	if (side.commander == player || side.candidate == player
		|| std::find(side.applicants.begin(), side.applicants.end(), player) != side.applicants.end()) {
		return;
	}
	if (side.candidate == -1 && side.applicants.empty()) {
		side.pickAt = m_time + kApplyPeriod;
	}
	side.applicants.push_back(player);
}

// Event 7 (0x004c5070 through SquadManager slot 0x6c): the best applicant
// (0x004c4fb0: the highest rank, the first of equals) leaves the list and
// is offered command (PostRemote 2/0x23; the HUD queues the question,
// 0x0079e440). One no longer on the team stays the candidate unasked, as
// in the original. The rewrite offers only while the team has no
// commander.
void GameWorld::offerCommand(int team)
{
	CommanderTeam& side = m_commanders[team];
	side.pickAt = -1;
	if (side.commander != -1) {
		return;
	}
	side.candidate = -1;
	auto best = side.applicants.end();
	int bestRank = -2;
	for (auto it = side.applicants.begin(); it != side.applicants.end(); ++it) {
		const Player* p = findPlayer(*it);
		if (p && p->rank > bestRank) {
			bestRank = p->rank;
			best = it;
		}
	}
	if (best == side.applicants.end()) {
		return;
	}
	side.candidate = *best;
	side.applicants.erase(best);
}

// The candidate's answer: kind 0 (SquadManager slot 0x68, 0x004c8c20)
// makes it the commander when it is the one offered; kind 1 (slot 0x6c,
// 0x004c5070) passes the offer to the next applicant at once.
void GameWorld::answerCommanderOffer(int player, bool accept)
{
	for (int team = 1; team <= 2; ++team) {
		CommanderTeam& side = m_commanders[team];
		if (side.candidate != player) {
			continue;
		}
		if (accept) {
			setCommander(team, player);
		} else {
			side.candidate = -1;
			offerCommand(team);
		}
	}
}

// Commander.commanderResign (slot 0x320; the server's kind 2, SquadManager
// slot 0x70 → 0x004c8a60): the commander steps down (PostRemote 0x25) and,
// with applicants waiting, the next is offered command 10 s later
// (SquadManager +0x18). An applicant resigning just leaves the list.
void GameWorld::resignCommander(int player)
{
	for (int team = 1; team <= 2; ++team) {
		CommanderTeam& side = m_commanders[team];
		side.applicants.erase(std::remove(side.applicants.begin(), side.applicants.end(), player), side.applicants.end());
		if (side.commander != player) {
			continue;
		}
		setCommander(team, -1);
		if (!side.applicants.empty()) {
			side.pickAt = m_time + kAfterResign;
		}
	}
}

// The objects holding a RemoteControlledObject, with their tool, their
// minTimeBetweenActivation and the GenericFireArm beside it (the strikes').
void GameWorld::scanCommanderAssets()
{
	m_assets.clear();
	for (const auto& owned : m_objects.objects()) {
		const world::ObjectTemplate* t = owned->objectTemplate();
		if (!t) {
			continue;
		}
		auto known = m_assetTemplates.find(t);
		if (known == m_assetTemplates.end()) {
			std::vector<CommanderAsset> found;
			std::string fireArm;
			for (const world::ChildTemplate& child : t->children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				if (c && c->typeName() == "GenericFireArm" && fireArm.empty()) {
					fireArm = c->name();
				}
			}
			for (const world::ChildTemplate& child : t->children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				CommanderAbility ability;
				if (!c || c->typeName() != "RemoteControlledObject") {
					continue;
				}
				const auto type = c->settings().find("rcType");
				if (type != c->settings().end() && abilityOfRcType(type->second, ability)) {
					found.push_back({0, ability, number(*c, "minTimeBetweenActivation", 0), fireArm});
				}
			}
			known = m_assetTemplates.emplace(t, std::move(found)).first;
		}
		for (CommanderAsset asset : known->second) {
			asset.object = owned->id();
			m_assets.push_back(asset);
		}
	}
}

bool GameWorld::isCommanderAsset(uint32_t object) const
{
	return std::any_of(m_assets.begin(), m_assets.end(), [object](const CommanderAsset& a) { return a.object == object; });
}

// An asset of the team for the tool that still stands. 0x005a1440 leaves
// out wrecks (the armour's slot 0x58); the assets can't be destroyed
// (armor.canBeDestroyed 0) but are wrecks with their hit points gone,
// until repaired.
const GameWorld::CommanderAsset* GameWorld::intactAsset(int team, CommanderAbility ability) const
{
	for (const CommanderAsset& asset : m_assets) {
		const world::Object* object = m_objects.find(asset.object);
		const float left = hitPoints(asset.object); // -1 without armour
		if (asset.ability == ability && object && object->team() == team && !object->destroyed() && (left > 0 || left == -1)) {
			return &asset;
		}
	}
	return nullptr;
}

// Recharges:
// - orbital strike and artillery: the largest minTimeBetweenActivation of
//   the team's standing assets (0x005a1440, in ticks); a use is refused
//   while less than that has passed since the last (0x005a1270);
// - UAV: it stays uavTime, then uavReloadTime (0x005a23b0);
// - satellite scan: satelliteScanTime, then satelliteScanReloadTime
//   (0x005a2450);
// - supply drop: "SupplyDrop-Reload-Time" (60 s), less 0.1 (0x0043d6d0).
float GameWorld::commanderRecharge(int team, CommanderAbility ability) const
{
	switch (ability) {
	case CommanderAbility::Uav:
		return commanderSettings.uavTime + commanderSettings.uavReloadTime;
	case CommanderAbility::Satellite:
		return commanderSettings.satelliteScanTime + commanderSettings.satelliteScanReloadTime;
	case CommanderAbility::Supplies:
		return commanderSettings.supplyDropReloadTime - 0.1f;
	default:
		break;
	}
	float recharge = 0;
	for (const CommanderAsset& asset : m_assets) {
		const world::Object* object = m_objects.find(asset.object);
		const float left = hitPoints(asset.object);
		if (asset.ability == ability && object && object->team() == team && !object->destroyed() && (left > 0 || left == -1)) {
			recharge = std::max(recharge, asset.minTimeBetweenActivation);
		}
	}
	return recharge;
}

GameWorld::CommanderAbilityState GameWorld::abilityState(int team, CommanderAbility ability) const
{
	CommanderAbilityState state;
	if (team != 1 && team != 2) {
		return state;
	}
	const CommanderTeam& side = m_commanders[team];
	const int index = static_cast<int>(ability);
	float recharge = commanderRecharge(team, ability);
	// A use counts against the recharge it had then.
	if (side.lastUse[index] > -1e8) {
		recharge = side.minTime[index];
	}
	if (ability == CommanderAbility::Supplies) {
		state.available = true;
		state.armor = 1;
	} else if (const CommanderAsset* asset = intactAsset(team, ability)) {
		state.available = true;
		state.asset = asset->object;
		const world::Object* object = m_objects.find(asset->object);
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		const float most = t ? number(*t, "armor.maxHitPoints", 0) : 0;
		state.armor = most > 0 ? std::clamp(hitPoints(asset->object) / most, 0.0f, 1.0f) : 1.0f;
	}
	const float since = static_cast<float>(m_time - side.lastUse[index]);
	state.timeUntilReady = std::max(0.0f, recharge - since);
	state.reloaded = recharge > 0 ? std::clamp(since / recharge, 0.0f, 1.0f) : 1.0f;
	state.ready = state.available && state.timeUntilReady <= 0;
	switch (ability) {
	case CommanderAbility::Uav:
		state.active = since < commanderSettings.uavTime;
		break;
	case CommanderAbility::Satellite:
		state.active = since < commanderSettings.satelliteScanTime;
		break;
	default:
		state.active = std::any_of(m_strikes.begin(), m_strikes.end(),
			[&](const Strike& s) { return s.team == team && s.ability == ability; });
		break;
	}
	return state;
}

// The server's side of the commander's tool event (vtable 0x0091e42c,
// handler 0x0050ee10): only the team's commander may use them, or the
// game itself (a sender that is no player: AIDLL's IBFCommander, byAi).
// - Orbital strike and artillery: 0x005a11c0 puts the target marker on
//   the ground (the higher of the terrain and the water) and 0x005a0bf0
//   moves the asset's firearm orbitalBombardmentHeight over it; the delay
//   is the firearm's launch delay in whole seconds (0x005c2220: the fire
//   component's slot 300, 0x005f16f0, reads fire.fireLaunchDelay at
//   template +0x1c; rounded as the x87 does, times 30 ticks); then
//   0x005a0e50 fires it straight down (0, -1, 0) for its burst (fire mode
//   7).
// - UAV: the point is kept (0x005a23b0) and the UAV stays uavTime.
// - Satellite scan: it runs satelliteScanTime.
// - Supply drop (0x0043d6d0, 0x0043d510): a supply_crate at the ground's
//   height plus supplyDropHeight; it goes after supplyDropNumSecsToLive
//   (0x0043d880).
bool GameWorld::commanderAction(int team, CommanderAbility ability, float x, float z, bool byAi)
{
	if ((team != 1 && team != 2) || (m_commanders[team].commander == -1 && !byAi)) {
		return false;
	}
	if (m_assetScan <= 0) {
		scanCommanderAssets();
		m_assetScan = 1;
	}
	const CommanderAbilityState state = abilityState(team, ability);
	if (!state.ready) {
		return false;
	}
	float ground = m_ground.height ? m_ground.height(x, z) : 0.0f;
	if (m_hasWater) {
		ground = std::max(ground, m_waterLevel);
	}
	const physics::Vec3 target {x, ground, z};
	CommanderTeam& side = m_commanders[team];
	const int index = static_cast<int>(ability);
	const CommanderAsset* asset = intactAsset(team, ability);
	switch (ability) {
	case CommanderAbility::OrbitalStrike:
	case CommanderAbility::Artillery: {
		const world::ObjectTemplate* fireArm = asset ? m_templates.find(asset->fireArm) : nullptr;
		if (!fireArm) {
			return false;
		}
		const float delay = std::nearbyint(number(*fireArm, "fire.fireLaunchDelay", 0));
		const int burst = std::max(1, integer(*fireArm, "fire.burstSize", 1));
		m_strikes.push_back({ability, team, target, delay, burst});
		m_strikeFire.push_back({fireArm->name(), side.commander});
		break;
	}
	case CommanderAbility::Uav:
		side.uavAt = target;
		break;
	case CommanderAbility::Satellite:
		break;
	case CommanderAbility::Supplies: {
		world::ObjectTemplate* crate = m_templates.find("supply_crate");
		if (!crate) {
			return false;
		}
		// It falls from the drop height when it has a body (physics on);
		// without, it comes down by itself (stepCrates).
		world::Object* placed = m_objects.create(crate, {x, ground + commanderSettings.supplyDropHeight, z});
		if (!placed) {
			return false;
		}
		placed->setTeam(team);
		const float storage = number(*crate, "sharedStorageSize", 0) + number(*crate, "healStorageSize", 0)
			+ number(*crate, "ammoStorageSize", 0);
		Crate dropped;
		dropped.object = placed->id();
		dropped.team = team;
		dropped.storage = storage;
		dropped.timeLeft = commanderSettings.supplyDropNumSecsToLive;
		m_crates.push_back(std::move(dropped));
		break;
	}
	}
	// The recharge it has now counts from now (0x005a1270 keeps the firing
	// tick and refuses until minTimeBetweenActivation has passed).
	side.minTime[index] = commanderRecharge(team, ability);
	side.lastUse[index] = m_time;
	return true;
}

bool GameWorld::uavArea(int team, physics::Vec3& at, float& radius) const
{
	if (!abilityState(team, CommanderAbility::Uav).active) {
		return false;
	}
	at = m_commanders[team].uavAt;
	radius = commanderSettings.uavRadius;
	return true;
}

bool GameWorld::satelliteScanActive(int team) const { return abilityState(team, CommanderAbility::Satellite).active; }

std::vector<int> GameWorld::scannedPlayers(int viewer) const
{
	std::vector<int> players;
	const Player* p = findPlayer(viewer);
	if (!p || (p->team != 1 && p->team != 2) || commander(p->team) != viewer || !satelliteScanActive(p->team)) {
		return players;
	}
	for (const auto& owned : m_players) {
		if (owned->alive && owned->team != p->team && (owned->team == 1 || owned->team == 2)) {
			players.push_back(owned->id);
		}
	}
	return players;
}

std::vector<uint32_t> GameWorld::supplyCrates() const
{
	std::vector<uint32_t> objects;
	for (const Crate& crate : m_crates) {
		objects.push_back(crate.object);
	}
	return objects;
}

// One round of a strike's burst: fire.batchSize projectiles (the
// asset's projectileTemplate at its velocity) from orbitalBombardmentHeight
// over the target, straight down (0x005a0e50 hands the firearm the
// direction (0, -1, 0)). The batch spreads by the firearm's
// SimpleDeviationComp (deviation.subProjectileDev, template +0x18 of
// 0x94e2); that component's object code is not rewritten, so here each
// round strays uniformly within subProjectileDev degrees of straight
// down. The ballistic solution 0x005a0750 only turns an artillery
// asset's barrels (mode +0x44 == 1, 0x005a0bf0); it doesn't change the
// shots' direction, so it is left out.
void GameWorld::fireStrike(Strike& strike, StrikeFire& fire)
{
	const world::ObjectTemplate* t = m_templates.find(fire.fireArm);
	if (!t) {
		return;
	}
	const FireArmSettings settings = FireArmSettings::fromTemplate(*t);
	const ProjectileSettings* aimed = projectileFor(settings.projectileTemplate);
	if (!aimed) {
		return;
	}
	// Their seek.targetType TTUnique comes with a FixedTargetComp and a
	// PreDestinedFollowComp: they hold for the fixed point below, not the
	// shooter's aim as the AV missile does; falling straight they reach it.
	std::unique_ptr<ProjectileSettings>& fixed = m_projectileSettings["strike:" + settings.projectileTemplate];
	if (!fixed) {
		fixed = std::make_unique<ProjectileSettings>(*aimed);
		fixed->seek = ProjectileSettings::Seek::None;
	}
	const ProjectileSettings* projectile = fixed.get();
	const int batch = std::max(1, integer(*t, "fire.batchSize", 1));
	const float spread = number(*t, "deviation.subProjectileDev", 0) * 3.14159265f / 180.0f;
	const physics::Vec3 from = strike.target + physics::Vec3 {0, commanderSettings.orbitalBombardmentHeight, 0};
	for (int i = 0; i < batch; ++i) {
		const float tilt = spread * m_random.unit();
		const float around = 2 * 3.14159265f * m_random.unit();
		const physics::Vec3 direction {std::sin(tilt) * std::cos(around), -std::cos(tilt), std::sin(tilt) * std::sin(around)};
		Projectile p;
		p.origin = from;
		p.position = from;
		p.velocity = direction * settings.velocity;
		p.settings = projectile;
		p.timeLeft = projectile->timeToLive.sample(m_random);
		p.player = fire.player;
		p.team = strike.team;
		m_projectiles.push_back(p);
	}
}

// Per tick: the assets are looked through once a second; an election
// due offers command; strikes count down and fire their bursts at the
// firearm's rate; crates fall and resupply. The UAV and the satellite
// scan reveal through the server's choice of what each client hears of
// (0x0057dc90's per-client scoping, 0x0057cfb0): while the team's UAV is
// up every player of the team gets the enemies (their control objects:
// the soldier, or the vehicle it rides) within uavRadius of the UAV's
// point, measured in 3-D (0x005a27c0 gives the point, gameLogic slot
// 0x28c the radius); while the scan runs (0x005a2370) the team's
// commander, and only the commander, gets every enemy (radius -1).
void GameWorld::stepCommander(float seconds)
{
	m_assetScan -= seconds;
	if (m_assetScan <= 0) {
		scanCommanderAssets();
		m_assetScan = 1;
	}
	for (int team = 1; team <= 2; ++team) {
		if (m_commanders[team].pickAt >= 0 && m_time >= m_commanders[team].pickAt) {
			offerCommand(team);
		}
	}
	for (size_t i = 0; i < m_strikes.size();) {
		Strike& strike = m_strikes[i];
		strike.fireIn -= seconds;
		while (strike.fireIn <= 0 && strike.salvosLeft > 0) {
			fireStrike(strike, m_strikeFire[i]);
			--strike.salvosLeft;
			const world::ObjectTemplate* t = m_templates.find(m_strikeFire[i].fireArm);
			const float rate = t ? number(*t, "fire.roundsPerMinute", 600) : 600;
			strike.fireIn += rate > 0 ? 60.0f / rate : 0.1f;
		}
		if (strike.salvosLeft <= 0) {
			m_strikes.erase(m_strikes.begin() + static_cast<long>(i));
			m_strikeFire.erase(m_strikeFire.begin() + static_cast<long>(i));
		} else {
			++i;
		}
	}
	for (int team = 1; team <= 2; ++team) {
		physics::Vec3 at;
		float radius = 0;
		if (!uavArea(team, at, radius)) {
			continue;
		}
		for (const auto& owned : m_players) {
			const Player& player = *owned;
			if (!player.alive || player.team == team) {
				continue;
			}
			const world::Object* object = m_objects.find(player.vehicle ? player.vehicle : player.object);
			if (!object) {
				continue;
			}
			const physics::Vec3 p {object->position().x, object->position().y, object->position().z};
			if (physics::lengthSquared(p - at) <= radius * radius) {
				float& left = m_revealed[{player.id, team}];
				left = std::max(left, 2 * seconds);
			}
		}
	}
	stepCrates(seconds);
}

// A supply crate (SupplyObject supply_crate; the object's update
// 0x005d1710 runs every 0.2 s on timer event 0x1d, 0x005d1900). The
// template (0x005d1940; saved by 0x005d10e0): radius +0x298,
// workOnVehicles +0x2a0, workOnSoldiers +0x2a1 (default 1), healSpeed
// +0x2a8, refillAmmoSpeed +0x2ac, healStorageSize +0x2b4 and
// ammoStorageSize +0x2b8 (-1: none), sharedStorageSize +0x2bc,
// destroyWhenEmptyStorage +0x2cc. Each update, for its team (or every
// team when it has none):
// - every soldier, and with workOnVehicles every vehicle, within the
//   crate's radius plus the target's own radius (0x005d0e80: the 3-D
//   distance between the objects' positions against object slot 0x90 +
//   radius; a soldier's radius is taken as 0 here, a vehicle's as its
//   farthest collision point) is
// - healed or repaired by healSpeed x 0.2 (0x005d0970: gameLogic slot
//   0x108, 0x004bc410/0x004cc380, scaled by the material table's cell for
//   materials 73 and 24, the same for every target), taking that much
//   from the storage when it took any, while the heal storage isn't spent
//   (0x005d0650: healSpeed x 0.2 left in it or the shared storage);
// - and each of its weapons gets refillAmmoSpeed x 0.2 per cent of a
//   magazine (0x005d0b20 → the ammo component's slot 0xa8, 0x005f5110:
//   magSize x amount / 100 rounds, the fraction kept), the amount taken
//   from the storage while the weapon isn't full.
// With destroyWhenEmptyStorage it goes once both storages are spent.
// Without a body it comes down at 5 m/s: its parachute (supply_parachute's
// Wing lift and supply_ParachuteForceDown's constantForce 5 on the rigid
// body) is only simulated when physics runs.
void GameWorld::stepCrates(float seconds)
{
	constexpr float kTick = 0.2f;
	const float factor = m_materials ? m_materials->damageMod(73, 24) : 1.0f;
	for (auto it = m_crates.begin(); it != m_crates.end();) {
		Crate& crate = *it;
		world::Object* object = m_objects.find(crate.object);
		crate.timeLeft -= seconds;
		if (!object || object->destroyed() || crate.timeLeft <= 0) {
			if (object) {
				m_objects.remove(crate.object);
			}
			it = m_crates.erase(it);
			continue;
		}
		const world::ObjectTemplate* t = object->objectTemplate();
		world::Vec3 p = object->position();
		if (!findBody(crate.object)) {
			const float ground = m_ground.height ? m_ground.height(p.x, p.z) : 0.0f;
			if (p.y > ground) {
				p.y = std::max(ground, p.y - 5.0f * seconds);
				object->setPosition(p);
			}
		}
		crate.tick -= seconds;
		if (crate.tick > 0) {
			++it;
			continue;
		}
		crate.tick += kTick;
		const physics::Vec3 at {p.x, p.y, p.z};
		const float radius = t ? number(*t, "radius", 2) : 2;
		const float heal = (t ? number(*t, "healSpeed", 0) : 0) * kTick;
		const float ammo = (t ? number(*t, "refillAmmoSpeed", 0) : 0) * kTick;
		// The heal storage is spent once less than a heal is left in it
		// and in the shared storage (the crate's own two are 0).
		const auto healSpent = [&] { return crate.storage < heal; };
		const auto mend = [&](uint32_t target, const world::ObjectTemplate* tt) {
			const float left = hitPoints(target);
			const float most = tt ? number(*tt, "armor.maxHitPoints", left) : left;
			if (heal <= 0 || healSpent() || left < 0 || left >= most) {
				return;
			}
			m_hitPoints[target] = std::min(most, left + heal * factor);
			crate.storage -= heal;
		};
		const auto refill = [&](FireArm* weapon) {
			if (!weapon || ammo <= 0 || crate.storage <= 0) {
				return;
			}
			float& owed = crate.owed[weapon];
			owed += ammo / 100.0f; // per cent of a magazine
			crate.storage -= ammo;
			while (owed >= 1) {
				owed -= 1;
				if (!weapon->addMagazine()) {
					owed = 0;
					crate.storage += ammo; // full: nothing taken
					break;
				}
			}
		};
		const bool soldiers = !t || integer(*t, "workOnSoldiers", 1) != 0;
		for (const auto& owned : m_players) {
			Player& target = *owned;
			if (!soldiers || !target.alive || target.vehicle || (crate.team != 0 && target.team != crate.team)) {
				continue;
			}
			const world::Object* soldier = m_objects.find(target.object);
			if (!soldier) {
				continue;
			}
			const world::Vec3 sp = soldier->position();
			if (physics::lengthSquared(physics::Vec3 {sp.x, sp.y, sp.z} - at) > radius * radius) {
				continue;
			}
			mend(target.object, soldier->objectTemplate());
			refill(target.weapon.get());
			for (Player::Carried& carried : target.carried) {
				refill(carried.weapon.get());
			}
		}
		if (t && integer(*t, "workOnVehicles", 0) != 0) {
			for (Body& body : m_bodies) {
				const world::Object* vehicle = m_objects.find(body.object);
				const world::ObjectTemplate* vt = vehicle ? vehicle->objectTemplate() : nullptr;
				if (!body.vehicle || body.wreckTime >= 0 || !vt || (crate.team != 0 && vehicle->team() != crate.team)) {
					continue;
				}
				float own = 0; // the body's radius: its farthest collision point
				for (const physics::Vec3& point : body.points) {
					own = std::max(own, std::sqrt(physics::lengthSquared(point)));
				}
				const float reach = radius + own;
				if (physics::lengthSquared(body.body.position - at) > reach * reach) {
					continue;
				}
				mend(body.object, vt);
				for (size_t g = 0; g < body.guns.size(); ++g) {
					refill(body.guns[g].weapon.get());
				}
			}
		}
		if (crate.storage <= 0 && t && integer(*t, "destroyWhenEmptyStorage", 0) != 0) {
			m_objects.remove(crate.object);
			it = m_crates.erase(it);
			continue;
		}
		++it;
	}
}

// The commander's order (Commander.sendOrder; SquadManager slot 0x4c,
// 0x004c5530 → the squad's commander order, 0x004c3e40): the commander
// hears HUD_HUD_SQUAD_ORDER_SENT_SQUAD (PostRemote 0x1b), the squad's
// leader HUD_HUD_SQUAD_RECIEVED_NEW_ORDER_COMMANDER (0x1a) and answers it.
bool GameWorld::issueOrder(int team, int squad, OrderType type, float x, float z, int from)
{
	if ((team != 1 && team != 2) || squadMembers(team, squad).empty()) {
		return false;
	}
	if (from != -1 && from != commander(team)) {
		return false;
	}
	const auto key = std::make_pair(team, squad);
	if (type == OrderType::None) {
		m_orders.erase(key);
		return true;
	}
	SquadOrder order;
	order.type = type;
	order.position = {x, m_ground.height ? m_ground.height(x, z) : 0.0f, z};
	order.fromCommander = true;
	m_orders[key] = order;
	return true;
}

// Commander.commRoseAccept: the squad leader takes the order, or turns it
// down: 0x004c2f60 sets the order's accepted flag and its valid flag to
// the answer, so a refused order goes.
void GameWorld::acceptOrder(int player, bool accept)
{
	const Player* p = findPlayer(player);
	if (!p || !p->squadLeader) {
		return;
	}
	const auto it = m_orders.find({p->team, p->squad});
	if (it == m_orders.end()) {
		return;
	}
	if (accept) {
		it->second.accepted = true;
	} else {
		m_orders.erase(it);
	}
}

const GameWorld::SquadOrder* GameWorld::squadOrder(int team, int squad) const
{
	const auto it = m_orders.find({team, squad});
	return it != m_orders.end() && !squadMembers(team, squad).empty() ? &it->second : nullptr;
}

// SquadLeader.sendOrder (SquadManager slot 0x48, 0x004c5360 → the
// squad's leader order, 0x004c3d90): the leader orders his own squad; the
// HUD sends it accepted (its +0x2b0 set before the send, 0x007d6ab0); the
// members hear HUD_HUD_SQUAD_RECIEVED_NEW_ORDER_SQUADLEADER (0x17), the
// leader HUD_HUD_SQUAD_ORDER_SENT_MEMBERS (0x1c). None drops it.
bool GameWorld::issueSquadLeaderOrder(int player, OrderType type, float x, float z)
{
	const Player* p = findPlayer(player);
	if (!p || !p->squadLeader || p->squad == 0 || (p->team != 1 && p->team != 2)) {
		return false;
	}
	const auto key = std::make_pair(p->team, p->squad);
	if (type == OrderType::None) {
		m_leaderOrders.erase(key);
		return true;
	}
	SquadOrder order;
	order.type = type;
	order.position = {x, m_ground.height ? m_ground.height(x, z) : 0.0f, z};
	order.fromCommander = false;
	order.accepted = true;
	m_leaderOrders[key] = order;
	return true;
}

const GameWorld::SquadOrder* GameWorld::squadLeaderOrder(int team, int squad) const
{
	const auto it = m_leaderOrders.find({team, squad});
	return it != m_leaderOrders.end() && !squadMembers(team, squad).empty() ? &it->second : nullptr;
}

} // namespace dice::hfe::game
