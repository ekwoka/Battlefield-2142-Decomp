#include "GameAudio.h"

#include "Game/GameWorld.h"
#include "Game/Vehicle.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/VoiceMessages.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dice::hfe {

namespace {

sound::Vec3 toSound(const physics::Vec3& v) { return {v.x, v.y, v.z}; }
sound::Vec3 toSound(const world::Vec3& v) { return {v.x, v.y, v.z}; }

// A setting of a template ("" when unset).
const std::string& setting(const world::ObjectTemplate& t, const char* key)
{
	static const std::string none;
	const auto it = t.settings().find(key);
	return it == t.settings().end() ? none : it->second;
}

// The owner of a loop: a kind in the top byte, then the object.
uint64_t ownerKey(int kind, uint64_t id) { return (static_cast<uint64_t>(kind) << 56) ^ id; }

} // namespace

GameAudio::GameAudio(game::GameWorld& round, world::ObjectManager& objects, world::ObjectTemplateManager& templates,
	sound::SoundManager& manager, sound::GameSounds& sounds, sound::VoiceMessages* voices)
	: m_round(round), m_objects(objects), m_templates(templates), m_manager(manager), m_sounds(sounds), m_voices(voices)
{
}

void GameAudio::effectSound(const std::string& name, const sound::Vec3& position) { m_sounds.play(name, position); }

void GameAudio::tick(const Camera& camera)
{
	m_manager.setListener(camera.position, camera.forward, camera.up, camera.velocity);
	autoRadio();
	for (const auto& sound : m_round.takeSounds()) {
		m_sounds.play(sound.sound, toSound(sound.position));
	}
	weapons(camera);
	engines();
	projectiles();
	radio();
	silos();
	if (m_voices) {
		// 0x004cdfb0: the world's effects duck under the help voice.
		m_manager.setMuted(3, m_voices->helpPlaying());
	}
	m_sounds.endTick();
	m_manager.update(1);
}

void GameAudio::weapons(const Camera& camera)
{
	const int local = m_round.localPlayer();
	for (const auto& event : m_round.takeWeaponSounds()) {
		++weaponSounds;
		const bool firstPerson = camera.firstPerson && event.player == local && local >= 0;
		const world::ObjectTemplate* weapon = m_templates.find(event.weapon);
		// sound.muteOnFire: on unless a script sets 0 (0x005dbc00 saves only
		// the 0).
		const bool muteOnFire = !weapon || setting(*weapon, "sound.muteOnFire") != "0";
		using Kind = game::GameWorld::WeaponSoundEvent::Kind;
		const sound::GameSounds::WeaponEvent kind = event.kind == Kind::Fire ? sound::GameSounds::WeaponEvent::Fire
			: event.kind == Kind::Reload                                  ? sound::GameSounds::WeaponEvent::Reload
			: event.kind == Kind::SwitchFireRate                          ? sound::GameSounds::WeaponEvent::SwitchFireRate
																		  : sound::GameSounds::WeaponEvent::Deploy;
		m_sounds.weapon(kind, event.weapon, firstPerson, toSound(event.position), muteOnFire);
	}
}

void GameAudio::engines()
{
	std::set<uint32_t> aboard;
	for (const auto& player : m_round.playerList()) {
		if (player.alive && player.vehicle) {
			aboard.insert(player.vehicle);
		}
	}
	for (const uint32_t id : aboard) {
		const game::Vehicle* vehicle = m_round.vehicle(id);
		const world::Object* object = m_objects.find(id);
		if (!vehicle || !object) {
			continue;
		}
		const sound::Vec3 at = toSound(object->position());
		const auto& parts = vehicle->parts();
		for (size_t index = 0; index < parts.size(); ++index) {
			const auto& part = parts[index];
			if (part.type != "Engine") {
				continue;
			}
			// The old engines run (+0x24c) and say their own rpm; newCar2's is
			// the vehicle's, on while someone is aboard.
			bool running = true;
			float rpm = vehicle->rpm();
			for (const auto& engine : vehicle->engines()) {
				if (engine.part == static_cast<int>(index)) {
					running = engine.running;
					rpm = engine.rpm;
				}
			}
			if (!running) {
				continue;
			}
			const float input = std::fabs(rpm * 0.8333333f);
			const uint64_t owner = ownerKey(1, (static_cast<uint64_t>(id) << 8) | index);
			for (const char* loop : {"_Idle", "_Rpm1", "_Rpm2"}) {
				m_sounds.loop(owner, "S_" + part.name + loop, at, {}, input);
			}
		}
	}
}

const std::string& GameAudio::loopingSound(const void*, const std::string& name)
{
	auto it = m_looping.find(name);
	if (it == m_looping.end()) {
		const std::string loop = "S_" + name + "_Looping";
		it = m_looping.emplace(name, m_manager.soundTemplate(loop) ? loop : std::string()).first;
	}
	return it->second;
}

void GameAudio::projectiles()
{
	for (const auto& projectile : m_round.projectiles()) {
		if (!projectile.alive || !projectile.settings || projectile.settings->name.empty()) {
			continue;
		}
		const std::string& loop = loopingSound(projectile.settings, projectile.settings->name);
		if (loop.empty()) {
			continue;
		}
		// Keyed by who fired it and where it left (it has no id of its own).
		uint32_t bits[3];
		const float origin[3] = {projectile.origin.x, projectile.origin.y, projectile.origin.z};
		std::memcpy(bits, origin, sizeof(bits));
		const uint64_t key = (static_cast<uint64_t>(projectile.shooter) << 32) ^ bits[0] ^ (static_cast<uint64_t>(bits[1]) << 11)
			^ (static_cast<uint64_t>(bits[2]) << 22);
		m_sounds.loop(ownerKey(2, key), loop, toSound(projectile.position), toSound(projectile.velocity));
	}
}

void GameAudio::radio()
{
	const int local = m_round.localPlayer();
	if (local < 0) {
		return;
	}
	const auto heard = m_round.takeRadioHeard([local](int receiver) { return receiver == local; });
	if (onHeard) {
		for (const auto& message : heard) {
			onHeard(message.sender, message.voice);
		}
	}
	if (!m_voices) {
		return;
	}
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(local, me)) {
		return;
	}
	for (const auto& message : heard) {
		++radioHeard;
		sound::VoiceMessages::Heard in;
		in.message = message.voice;
		in.listenerTeam = me.team;
		in.listener = toSound(me.position);
		game::GameWorld::PlayerInfo sender;
		if (message.sender >= 0 && m_round.playerInfo(message.sender, sender)) {
			in.hasSender = true;
			in.senderIsListener = message.sender == local;
			in.commander = sender.commander;
			in.squadLeader = sender.squadLeader;
			in.onFoot = sender.alive && sender.vehicle == 0;
			in.sender.id = sender.id;
			in.sender.team = sender.team;
			in.sender.position = toSound(sender.position) + sound::Vec3 {0, 1.6f, 0};
		}
		if (m_voices->heard(in).played != sound::VoiceMessages::Played::None) {
			++voiceOvers;
		}
	}
}

void GameAudio::silos()
{
	for (const auto& silo : m_round.missileSilos()) {
		auto found = m_silos.find(silo.object);
		if (found == m_silos.end()) {
			SiloSounds sounds;
			// The firearm in its tree with the countdown (CountdownSoundComp).
			const world::Object* object = m_objects.find(silo.object);
			std::vector<const world::ObjectTemplate*> open;
			if (object && object->objectTemplate()) {
				open.push_back(object->objectTemplate());
			}
			for (size_t i = 0; i < open.size() && i < 64 && sounds.weapon.empty(); ++i) {
				for (const auto& [key, value] : open[i]->settings()) {
					if (key.compare(0, 22, "sound.reloadTimeSound.") == 0) {
						sounds.weapon = open[i]->name();
						sounds.times.push_back({std::atoi(key.c_str() + 22), static_cast<float>(std::atof(value.c_str()))});
					}
				}
				for (const auto& child : open[i]->children()) {
					if (const world::ObjectTemplate* t = m_templates.find(child.name)) {
						open.push_back(t);
					}
				}
			}
			found = m_silos.emplace(silo.object, std::move(sounds)).first;
		}
		SiloSounds& sounds = found->second;
		if (!sounds.weapon.empty() && sounds.lastLeft >= 0 && (silo.team == 1 || silo.team == 2)) {
			for (const auto& [index, seconds] : sounds.times) {
				if (sounds.lastLeft > seconds && silo.reloadLeft <= seconds) {
					m_sounds.play("S_" + sounds.weapon + "_ReloadTimeTeam" + std::to_string(silo.team) + "_"
							+ std::to_string(index),
						toSound(silo.position));
				}
			}
		}
		sounds.lastLeft = silo.loaded ? -1 : silo.reloadLeft;
	}
}

std::vector<std::pair<int, std::string>> GameAudio::autoRadio()
{
	std::vector<std::pair<int, std::string>> sent;
	const auto send = [&](int team, const std::string& message) {
		if ((team == 1 || team == 2) && !message.empty()) {
			m_round.radioMessage(-1, 13, message, team); // TCServerTeamMessage
			sent.push_back({team, message});
		}
	};
	const int local = m_round.localPlayer();
	game::GameWorld::PlayerInfo me;
	const int localTeam = m_round.playerInfo(local, me) ? me.team : 0;
	const auto play = [&](const std::string& message) {
		// Game slot 0x60 (0x0042e820): for the local player only.
		if (local >= 0 && !message.empty()) {
			m_round.hearRadio(local, 13, message);
			sent.push_back({localTeam, message});
		}
	};
	const auto teamOf = [&](const world::Object& object) {
		const int team = object.team();
		return team != 0 ? team : localTeam; // 0x00656910's last resort
	};
	// Takeable / not takeable (Radio slots 0x60 0x00656fc0 and 0x64
	// 0x006567b0, from the point's allowCaptureByTeam setter 0x005bcd00):
	// only a point that needs supply groups for that team, team 1 or 2;
	// for the local player, friendly when that is its team; after
	// radio.delay whole seconds (template +0x218, an int) through timed
	// event 2/0x52, whose handler (0x00758cd0 in the HUD manager's
	// 0x00756b60) plays it as it is, else at once (game slot 0x60).
	for (auto it = m_delayedRadio.begin(); it != m_delayedRadio.end();) {
		it->first -= 1.0f / 30;
		if (it->first <= 0) {
			play(it->second);
			it = m_delayedRadio.erase(it);
		} else {
			++it;
		}
	}
	for (const game::GameWorld::TakeableChange& c : m_round.takeTakeableChanges()) {
		if ((c.team != 1 && c.team != 2) || local < 0) {
			continue;
		}
		for (const game::ControlPoint& cp : m_round.controlPoints()) {
			if (cp.id != c.point || cp.supplyGroupsNeeded[c.team].empty() || !cp.object || !cp.object->objectTemplate()) {
				continue;
			}
			const world::ObjectTemplate& t = *cp.object->objectTemplate();
			const bool friendly = localTeam == c.team;
			const std::string message = setting(t,
				c.allowed ? (friendly ? "radio.friendlyTakeableMessage" : "radio.enemyTakeableMessage")
						  : (friendly ? "radio.friendlyNotTakeableMessage" : "radio.enemyNotTakeableMessage"));
			const int delay = std::atoi(setting(t, "radio.delay").c_str());
			if (message.empty()) {
				break;
			}
			if (delay > 0) {
				m_delayedRadio.push_back({static_cast<float>(delay), message});
			} else {
				play(message);
			}
			break;
		}
	}
	if (--m_scanIn <= 0) {
		// The objects with radio messages, looked for every two seconds.
		m_scanIn = 60;
		for (const auto& object : m_objects.objects()) {
			const world::ObjectTemplate* t = object->objectTemplate();
			if (!t || m_watched.count(object->id())) {
				continue;
			}
			auto known = m_radioTemplates.find(t);
			if (known == m_radioTemplates.end()) {
				const auto& s = t->settings();
				bool any = false;
				for (const char* key : {"radio.friendlyDestroyedMessage", "radio.destroyedMessage",
						 "radio.enemyDestroyedMessage", "radio.friendlyTeamChangeMessage", "radio.enemyTeamChangeMessage",
						 "radio.neutralTeamChangeMessage", "radio.damageMessage", "radio.repairedMessage",
						 "radio.friendlySpawnMessage", "radio.enemySpawnMessage"}) {
					any = any || s.count(key);
				}
				known = m_radioTemplates.emplace(t, any).first;
			}
			if (!known->second) {
				continue;
			}
			Watched w;
			w.team = object->team();
			w.hitPoints = m_round.hitPoints(object->id());
			w.destroyed = w.hitPoints >= 0 && w.hitPoints <= 0;
			w.objectTemplate = t;
			m_watched[object->id()] = w;
			if (!m_firstScan) {
				// Made since the round's start (the server's createObject).
				const int team = teamOf(*object);
				send(team, setting(*t, "radio.friendlySpawnMessage"));
				if (team == 1 || team == 2) {
					send(3 - team, setting(*t, "radio.enemySpawnMessage"));
				}
			}
		}
		m_firstScan = false;
	}
	for (auto it = m_watched.begin(); it != m_watched.end();) {
		const world::Object* object = m_objects.find(it->first);
		Watched& watched = it->second;
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		if (!t) {
			// Gone (a wreck removed) before its end was seen: destroyed.
			if (watched.objectTemplate && !watched.destroyed && watched.destroyedArmed && watched.hitPoints >= 0) {
				const world::ObjectTemplate& was = *watched.objectTemplate;
				const int team = watched.team != 0 ? watched.team : localTeam;
				std::string friendly = setting(was, "radio.friendlyDestroyedMessage");
				if (friendly.empty()) {
					friendly = setting(was, "radio.destroyedMessage");
				}
				send(team, friendly);
				if (team == 1 || team == 2) {
					send(3 - team, setting(was, "radio.enemyDestroyedMessage"));
				}
			}
			it = m_watched.erase(it);
			continue;
		}
		if (object->team() != watched.team) {
			const int to = object->team(), from = watched.team;
			// A silo's index (the HUD's slot: its control point, else its
			// place among the silos) as SiloSyncEvent hands it.
			int silo = 0;
			const auto silos = m_round.missileSilos();
			for (size_t i = 0; i < silos.size(); ++i) {
				if (silos[i].object == it->first) {
					silo = silos[i].controlPoint > 0 ? silos[i].controlPoint : static_cast<int>(i) + 1;
				}
			}
			const auto suffixed = [&](const std::string& message) {
				return message.empty() ? message : message + siloSuffix(silo);
			};
			if (to == 0) {
				if (localTeam == from) {
					play(suffixed(setting(*t, "radio.neutralTeamChangeMessage")));
				}
			} else {
				play(suffixed(setting(*t, localTeam == to ? "radio.friendlyTeamChangeMessage" : "radio.enemyTeamChangeMessage")));
			}
			watched.team = to;
		}
		const float hp = m_round.hitPoints(it->first);
		const int team = teamOf(*object);
		if (hp >= 0) {
			if (watched.hitPoints >= 0 && hp < watched.hitPoints && hp > 0 && watched.damageArmed) {
				watched.damageArmed = false;
				send(team, setting(*t, "radio.damageMessage"));
			}
			const bool destroyed = hp <= 0;
			if (destroyed && !watched.destroyed && watched.destroyedArmed) {
				watched.destroyedArmed = false;
				watched.repairedArmed = true;
				std::string friendly = setting(*t, "radio.friendlyDestroyedMessage");
				if (friendly.empty()) {
					friendly = setting(*t, "radio.destroyedMessage");
				}
				send(team, friendly);
				if (team == 1 || team == 2) {
					send(3 - team, setting(*t, "radio.enemyDestroyedMessage"));
				}
			} else if (!destroyed && watched.destroyed && watched.repairedArmed) {
				watched.repairedArmed = false;
				watched.damageArmed = watched.destroyedArmed = true;
				send(team, setting(*t, "radio.repairedMessage"));
			}
			watched.destroyed = destroyed;
			watched.hitPoints = hp;
		}
		++it;
	}
	return sent;
}

} // namespace dice::hfe
