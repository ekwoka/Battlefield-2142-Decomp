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
	if (--m_scanIn <= 0) {
		// The objects with radio messages, looked for every two seconds.
		m_scanIn = 60;
		for (const auto& object : m_objects.objects()) {
			const world::ObjectTemplate* t = object->objectTemplate();
			if (!t || m_watched.count(object->id())) {
				continue;
			}
			const auto& s = t->settings();
			if (s.count("radio.friendlyDestroyedMessage") || s.count("radio.enemyDestroyedMessage")
				|| s.count("radio.friendlyTeamChangeMessage") || s.count("radio.enemyTeamChangeMessage")
				|| s.count("radio.neutralTeamChangeMessage")) {
				m_watched[object->id()] = {object->team(), m_round.hitPoints(object->id()) <= 0};
			}
		}
	}
	for (auto it = m_watched.begin(); it != m_watched.end();) {
		const world::Object* object = m_objects.find(it->first);
		Watched& watched = it->second;
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		const bool destroyed = !object || m_round.hitPoints(it->first) <= 0;
		if (t && object->team() != watched.team) {
			const int to = object->team();
			if (to == 1 || to == 2) {
				send(to, setting(*t, "radio.friendlyTeamChangeMessage"));
				send(3 - to, setting(*t, "radio.enemyTeamChangeMessage"));
			} else {
				send(watched.team, setting(*t, "radio.neutralTeamChangeMessage"));
			}
			watched.team = to;
		}
		if (destroyed && !watched.destroyed && t) {
			send(watched.team, setting(*t, "radio.friendlyDestroyedMessage"));
			if (watched.team == 1 || watched.team == 2) {
				send(3 - watched.team, setting(*t, "radio.enemyDestroyedMessage"));
			}
		}
		watched.destroyed = destroyed;
		it = object ? std::next(it) : m_watched.erase(it);
	}
	return sent;
}

} // namespace dice::hfe
