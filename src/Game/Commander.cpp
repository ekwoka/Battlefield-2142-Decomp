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

float acrossSquared(const physics::Vec3& a, const physics::Vec3& b)
{
	return (a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z);
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
// one leaves its squad (0x004c7bb0) and takes it, the clients hear of it,
// and the scripts get ChangedCommander (event 10: team, old, new).
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
		side.applicants.erase(std::remove(side.applicants.begin(), side.applicants.end(), player), side.applicants.end());
	}
	raise("ChangedCommander", {ScriptValue::integer(team), ScriptValue::player(old), ScriptValue::player(player)});
	return true;
}

// Commander.commanderApply (HudManager slot 0x31c): the player joins the
// team's applicants; with no commander the best applicant takes over
// (0x004c4fb0 picks the highest rank, the first of equals). Provisional:
// there are no ranks without stats, so the first applicant wins, and at
// once (the original waits out an apply period,
// HUD_COMMANDERAPPLY_TIMETOSTART, not traced). The bots' own commander
// (AIDLL's IBFCommander user) applies only for a team without one.
void GameWorld::applyForCommander(int player)
{
	const Player* p = findPlayer(player);
	if (!m_commanderEnabled || !p || (p->team != 1 && p->team != 2)) {
		return;
	}
	CommanderTeam& side = m_commanders[p->team];
	if (side.commander == player || std::find(side.applicants.begin(), side.applicants.end(), player) != side.applicants.end()) {
		return;
	}
	side.applicants.push_back(player);
	// Provisional: a bot commanding steps down for a human applicant.
	const Player* current = findPlayer(side.commander);
	if (current && current->ai && !p->ai) {
		setCommander(p->team, player);
		return;
	}
	if (side.commander == -1) {
		const int next = side.applicants.front();
		setCommander(p->team, next);
	}
}

// Commander.commanderResign (slot 0x320; SquadManager slot 0x68 clears the
// commander when it is the one resigning), then the next applicant
// (0x004c5070). Provisional: the next applicant takes over at once instead
// of being asked first (event 0x23).
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
			setCommander(team, side.applicants.front());
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
// handler 0x0050ee10): only the team's commander may use them.
// - Orbital strike and artillery: 0x005a11c0 puts the target marker on
//   the ground (the higher of the terrain and the water) and 0x005a0bf0
//   moves the asset's firearm orbitalBombardmentHeight over it; the delay
//   is the firearm's launch delay in whole seconds (slot 300, provisional:
//   fire.fireLaunchDelay, rounded as the x87 does); then 0x005a0e50 fires
//   it straight down (0, -1, 0) for its burst (fire mode 7).
// - UAV: the point is kept (0x005a23b0) and the UAV stays uavTime.
// - Satellite scan: it runs satelliteScanTime.
// - Supply drop (0x0043d6d0, 0x0043d510): a supply_crate at the ground's
//   height plus supplyDropHeight; it goes after supplyDropNumSecsToLive
//   (0x0043d880).
bool GameWorld::commanderAction(int team, CommanderAbility ability, float x, float z)
{
	if ((team != 1 && team != 2) || m_commanders[team].commander == -1) {
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
		m_crates.push_back({placed->id(), team, storage, commanderSettings.supplyDropNumSecsToLive});
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
// over the target, straight down. Provisional: each strays from straight
// down by up to deviation.subProjectileDev degrees (the engine's spread of
// sub-projectiles isn't traced), and the artillery's ballistic aim
// (0x005a0750) is left out: from that height its shells fall almost
// straight anyway.
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

// Per tick: the assets are looked through once a second; strikes count
// down and fire their bursts at the firearm's rate; a UAV reveals the
// enemies within its radius across the ground to its team while it is up
// (provisional: what the UAV shows is taken from the minimap's ring,
// uavRadius); a satellite scan reveals every enemy while it runs
// (provisional); crates fall and resupply.
void GameWorld::stepCommander(float seconds)
{
	m_assetScan -= seconds;
	if (m_assetScan <= 0) {
		scanCommanderAssets();
		m_assetScan = 1;
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
		const bool uav = uavArea(team, at, radius);
		const bool scan = satelliteScanActive(team);
		if (!uav && !scan) {
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
			if (scan || acrossSquared(p, at) <= radius * radius) {
				float& left = m_revealed[{player.id, team}];
				left = std::max(left, 2 * seconds);
			}
		}
	}
	stepCrates(seconds);
}

// A supply crate (SupplyObject supply_crate): within `radius` it heals
// its team's soldiers by healSpeed and refills their ammunition by
// refillAmmoSpeed a second (provisional units: hit points, and a magazine
// for every 10 as with the ammo hubs), and with workOnVehicles mends
// vehicles by healSpeed; all from its shared storage
// (sharedStorageSize), and with destroyWhenEmptyStorage it goes once that
// is spent. Without a body it comes down at 5 m/s (provisional: its
// parachute, supply_ParachuteForceDown's constantForce 5, isn't
// simulated).
void GameWorld::stepCrates(float seconds)
{
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
		const physics::Vec3 at {p.x, p.y, p.z};
		const float radius = t ? number(*t, "radius", 5) : 5;
		const float heal = (t ? number(*t, "healSpeed", 0) : 0) * seconds;
		const float ammo = (t ? number(*t, "refillAmmoSpeed", 0) : 0) * seconds;
		for (const auto& owned : m_players) {
			Player& target = *owned;
			if (!target.alive || target.vehicle || target.team != crate.team || crate.storage <= 0) {
				continue;
			}
			const physics::Vec3 feet = target.body.position - physics::Vec3 {0, 1.0f, 0};
			if (physics::lengthSquared(feet - at) > radius * radius) {
				continue;
			}
			const world::Object* soldier = m_objects.find(target.object);
			const world::ObjectTemplate* st = soldier ? soldier->objectTemplate() : nullptr;
			const float left = hitPoints(target.object);
			const float most = st ? number(*st, "armor.maxHitPoints", left) : left;
			if (left >= 0 && left < most) {
				const float given = std::min({heal, most - left, crate.storage});
				m_hitPoints[target.object] = left + given;
				crate.storage -= given;
			}
			if (target.weapon && ammo > 0) {
				float& owed = m_resupply[target.object];
				owed += ammo / 10.0f;
				while (owed >= 1 && crate.storage > 0) {
					owed -= 1;
					if (target.weapon->addMagazine()) {
						crate.storage -= 10;
					} else {
						owed = 0;
					}
				}
			}
		}
		if (t && integer(*t, "workOnVehicles", 0) != 0) {
			for (const Body& body : m_bodies) {
				const world::Object* vehicle = m_objects.find(body.object);
				const world::ObjectTemplate* vt = vehicle ? vehicle->objectTemplate() : nullptr;
				if (!body.vehicle || body.wreckTime >= 0 || !vt || crate.storage <= 0
					|| physics::lengthSquared(body.body.position - at) > radius * radius) {
					continue;
				}
				const float left = hitPoints(body.object);
				const float most = number(*vt, "armor.maxHitPoints", left);
				if (left >= 0 && left < most) {
					const float given = std::min({heal, most - left, crate.storage});
					m_hitPoints[body.object] = left + given;
					crate.storage -= given;
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

// Orders (Commander.sendOrder <type> <bool>, issueOrder; the order
// marker's position, orderRadius and the leader's acceptance reach the
// scripts as orderPosition, orderRadius, ifAcceptedOrder and
// ifAcceptedAttackDefendOrder).
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
// down (it goes).
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

} // namespace dice::hfe::game
