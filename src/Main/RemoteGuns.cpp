#include "RemoteGuns.h"

#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace dice::hfe::client {

const game::GameWorld::SeatWeapon* RemoteSeatGuns::groupWeapon(const game::GameWorld::SeatLayout& seat, int group, int place)
{
	const int wanted = std::max(0, place);
	int n = 0;
	for (const game::GameWorld::SeatWeapon& w : seat.weapons) {
		if (w.group != group || w.weapon.empty()) {
			continue;
		}
		if (n++ == wanted) {
			return &w;
		}
	}
	return nullptr;
}

void RemoteSeatGuns::step(game::GameWorld& round, const std::vector<Seat>& seats, float seconds)
{
	std::set<std::pair<int, int>> seen;
	for (const Seat& s : seats) {
		const world::Object* vehicle = round.objects().find(s.vehicle);
		if (!vehicle) {
			continue;
		}
		const std::vector<game::GameWorld::SeatLayout> layout = round.vehicleSeats(s.vehicle);
		if (s.seat < 0 || static_cast<size_t>(s.seat) >= layout.size()) {
			continue;
		}
		for (int g = 0; g < 3; ++g) {
			const game::GameWorld::SeatWeapon* w = groupWeapon(layout[static_cast<size_t>(s.seat)], g, s.active[g]);
			if (!w) {
				continue;
			}
			const std::pair<int, int> key {s.player, g};
			seen.insert(key);
			Gun& gun = m_guns[key];
			if (gun.weapon != w->weapon) {
				// The group's active weapon changed (or a new seat): its
				// magazine starts full.
				gun = Gun {};
				gun.weapon = w->weapon;
				if (const world::ObjectTemplate* t = round.templates().find(w->weapon)) {
					gun.arm = game::FireArmSettings::fromTemplate(*t);
				}
				gun.rounds = gun.arm.magSize > 0 ? gun.arm.magSize : 1;
				gun.presses = s.presses;
				gun.deviation = w->deviation;
				gun.hasDeviation = w->hasDeviation;
			}
			// The deviation's tick (0x005dedb0): the firing's build-up
			// decays off the trigger.
			if (gun.hasDeviation) {
				gun.deviation.update(s.firing[g]);
			}
			const game::FireArmSettings& arm = gun.arm;
			// The server's rounds while it fires (0x00595c60 hands them to
			// the group's firearm): a reload done there ends ours.
			if (s.firing[g] && s.rounds[g] >= 0) {
				if (s.rounds[g] > gun.rounds) {
					gun.reloadLeft = 0;
				}
				gun.rounds = s.rounds[g];
			}
			bool pressed = s.firing[g] && !gun.wasFiring;
			if (g == 0 && s.presses >= 0 && gun.presses >= 0 && s.presses != gun.presses) {
				pressed = true; // a press between two updates (+0x24c)
			}
			if (g == 0) {
				gun.presses = s.presses;
			}
			gun.wasFiring = s.firing[g];
			const int mode = s.fireMode[g] >= 0 ? s.fireMode[g]
				: arm.fireRates.empty()        ? game::kFireAutomatic
											   : arm.fireRates.front();
			if (pressed && mode != game::kFireAutomatic) {
				gun.burstLeft = mode == game::kFireBurst ? std::max(1, arm.burstSize) : 1;
			}
			gun.nextRound = std::max(0.0f, gun.nextRound - seconds);
			if (gun.reloadLeft > 0) {
				gun.reloadLeft -= seconds;
				if (gun.reloadLeft <= 0) {
					gun.reloadLeft = 0;
					gun.rounds = arm.magSize > 0 ? arm.magSize : 1;
				}
				continue;
			}
			if (gun.nextRound > 0 || gun.rounds == 0
				|| !(mode == game::kFireAutomatic ? s.firing[g] : gun.burstLeft > 0)) {
				continue;
			}
			// A round: its projectile from the muzzle, its sound there and
			// event 0 to the gun's children.
			const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll(
				{vehicle->rotation().x, vehicle->rotation().y, vehicle->rotation().z});
			physics::Vec3 at = physics::Vec3 {vehicle->position().x, vehicle->position().y, vehicle->position().z}
				+ turn.transform(w->weaponPosition);
			float deviation = 0;
			if (gun.hasDeviation) {
				deviation = gun.deviation.deviation();
				gun.deviation.fired();
			}
			round.fireRemoteSeatRound(s.vehicle, s.seat, w->weapon, s.velocity, gun.barrel, gun.tracerCount, &at, deviation);
			round.remoteGunFired(s.vehicle, w->weapon, s.player, at, s.seat);
			gun.nextRound = arm.roundsPerMinute > 0 ? 60.0f / static_cast<float>(arm.roundsPerMinute) : 0.1f;
			gun.burstLeft = std::max(0, gun.burstLeft - 1);
			if (arm.magSize > 0 && gun.rounds > 0) {
				--gun.rounds;
			}
			// Only ammo.autoReload reloads by itself (0x005f45e0 / 0x005f4a40).
			if (gun.rounds == 0 && arm.magSize > 0 && arm.autoReload) {
				gun.reloadLeft = arm.reloadTime;
			}
		}
	}
	for (auto it = m_guns.begin(); it != m_guns.end();) {
		it = seen.count(it->first) ? std::next(it) : m_guns.erase(it);
	}
}

namespace {

std::string findFireArm(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t, int depth)
{
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* c = templates.find(child.name);
		if (!c) {
			continue;
		}
		if (c->typeName() == "GenericFireArm") {
			return c->name();
		}
		if (depth < 16) {
			if (std::string below = findFireArm(templates, *c, depth + 1); !below.empty()) {
				return below;
			}
		}
	}
	return {};
}

} // namespace

std::string RemoteStrikes::firstFireArm(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& asset)
{
	return findFireArm(templates, asset, 0);
}

void RemoteStrikes::fired(const world::ObjectTemplateManager& templates, uint32_t asset,
	const world::ObjectTemplate& assetTemplate, float x, float z, float height)
{
	Burst b;
	b.round.asset = asset;
	b.round.fireArm = firstFireArm(templates, assetTemplate);
	if (b.round.fireArm.empty()) {
		return; // 0x0050eeb0: no weapon in the list, nothing fires
	}
	b.round.at = {x, height, z};
	if (const world::ObjectTemplate* arm = templates.find(b.round.fireArm)) {
		const auto setting = [&](const char* key, float fallback) {
			const auto it = arm->settings().find(key);
			return it != arm->settings().end() ? static_cast<float>(std::atof(it->second.c_str())) : fallback;
		};
		b.left = std::max(1, static_cast<int>(setting("fire.burstSize", 1)));
		const float rate = setting("fire.roundsPerMinute", 600);
		b.interval = rate > 0 ? 60.0f / rate : 0.1f;
	} else {
		b.left = 1;
	}
	m_bursts.push_back(b);
	if (m_bursts.size() > 16) {
		m_bursts.erase(m_bursts.begin());
	}
}

std::vector<RemoteStrikes::Round> RemoteStrikes::step(float seconds)
{
	std::vector<Round> due;
	for (Burst& b : m_bursts) {
		b.next -= seconds;
		while (b.left > 0 && b.next <= 0) {
			due.push_back(b.round);
			--b.left;
			b.next += b.interval;
		}
	}
	m_bursts.erase(std::remove_if(m_bursts.begin(), m_bursts.end(), [](const Burst& b) { return b.left <= 0; }), m_bursts.end());
	return due;
}

} // namespace dice::hfe::client
