#include "GameAudio.h"

#include "Game/GameWorld.h"
#include "HudWorld.h"
#include "Game/Vehicle.h"
#include "Physics/MaterialManager.h"
#include "Console/Vars.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/VoiceMessages.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
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

float number(const world::ObjectTemplate& t, const char* key, float fallback)
{
	const std::string& value = setting(t, key);
	return value.empty() ? fallback : static_cast<float>(std::atof(value.c_str()));
}

// Whether a point is within a vehicle's box (its collision LOD's, in its
// own frame).
bool insideBox(const game::GameWorld::VehicleInfo& v, const physics::Vec3& p)
{
	const physics::Vec3 d = p - v.position;
	const physics::Vec3& f = v.forward;
	const physics::Vec3& r = v.right;
	const physics::Vec3 up {f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
	const float x = d.x * r.x + d.y * r.y + d.z * r.z;
	const float y = d.x * up.x + d.y * up.y + d.z * up.z;
	const float z = d.x * f.x + d.y * f.y + d.z * f.z;
	return x >= v.boxMin.x && x <= v.boxMax.x && y >= v.boxMin.y && y <= v.boxMax.y && z >= v.boxMin.z
		&& z <= v.boxMax.z;
}

// The soldier's own material against the floors the shipped cells pair
// (Human_body, 24, in materialManagerDefine.con): 0x00586a90's param_6.
constexpr uint32_t kSoldierBodyMaterial = 24;

} // namespace

GameAudio::~GameAudio() { releaseInterior(); }

void GameAudio::releaseInterior()
{
	m_manager.setInteriorAmbient(nullptr, 0);
	for (sound::ISound** sound : {&m_interior.ambient, &m_interior.damaged}) {
		if (*sound) {
			(*sound)->stop();
			(*sound)->setVolumeEnvelope(nullptr, nullptr);
			(*sound)->release();
			*sound = nullptr;
		}
	}
	m_interior = Interior {};
}

// The listener as the sound manager's update (0x007f9bf0 -> 0x007f9d50)
// takes it: the velocity only in first person (the local player's
// object's root's, from its physics, slot 0x58), the root kept for the
// obstruction rays (+0xdc), inside a Titan (the player's slot 0x390: the
// rewrite's, a Titan seat or within a Titan's box on foot), the wind (on
// foot, 0x007fda00, or in a seat whose listenerObstruction is 0,
// 0x005c7dc0; never inside a Titan or out of first person) and the seat's
// interior loop.
void GameAudio::listener(Camera& camera)
{
	game::GameWorld::PlayerInfo me;
	const bool alive = playerInfo(localPlayer(), me) && me.alive;
	if (!camera.firstPerson) {
		camera.velocity = {};
	}
	bool titanSeat = false, inside = false;
	if (alive && me.vehicle) {
		titanSeat = m_round.isTitan(me.vehicle);
		inside = titanSeat;
	} else if (alive && !m_round.titans().empty()) {
		for (const auto& v : m_round.vehicleList()) {
			if (m_round.isTitan(v.object) && insideBox(v, me.position + physics::Vec3 {0, 1.0f, 0})) {
				inside = true;
				break;
			}
		}
	}
	m_listenerState.insideTitan = inside;
	m_manager.setListenerRoot(alive ? (me.vehicle ? me.vehicle : me.soldier) : 0, titanSeat);
	interior(me, alive && camera.firstPerson);
	m_manager.setWindAllowed(alive && camera.firstPerson && !inside);
	const bool wind = alive && (!me.vehicle || std::fabs(m_interior.obstruction) <= 1.1920929e-07f);
	m_listenerState.windWanted = wind;
	if (wind) {
		m_manager.startWind();
	} else {
		m_manager.stopWind();
	}
	soldierHud(me, alive);
}

// A PlayerControlObject the local player sits in, seen from its eyes
// (0x005c7dc0, 0x005c8a90 and the update 0x005c9xxx): its Ambient loop
// as the engine's global ambient (slot 30), AmbientDamaged instead while
// the root's armour is under damagedAmbientSoundLimit (template +0x478,
// default 0.5) of its maximum, both read at +0x304 = clamp((|v| - 0.1)
// / 30, 0, 1) of the root's speed; the engine's listener-3d-obstruction
// tweak at its listenerObstruction (template +0x298 slot 0x11c). Out of
// it (or out of first person) none and 0.
void GameAudio::interior(const game::GameWorld::PlayerInfo& me, bool firstPerson)
{
	const uint32_t vehicle = firstPerson ? me.vehicle : 0;
	const int seat = vehicle ? me.seat : -2;
	if (vehicle != m_interior.vehicle || seat != m_interior.seat) {
		releaseInterior();
		m_interior.vehicle = vehicle;
		m_interior.seat = seat;
		if (vehicle) {
			const auto seats = m_round.vehicleSeats(vehicle);
			if (seat >= 0 && static_cast<size_t>(seat) < seats.size()) {
				m_interior.pco = seats[static_cast<size_t>(seat)].name;
			}
			const world::ObjectTemplate* t = m_templates.find(m_interior.pco);
			if (t) {
				m_interior.obstruction = number(*t, "listenerObstruction", 0);
				m_interior.damagedLimit = number(*t, "damagedAmbientSoundLimit", 0.5f);
			}
			if (const world::Object* root = m_objects.find(vehicle); root && root->objectTemplate()) {
				const world::ObjectTemplate& rt = *root->objectTemplate();
				m_interior.maxHitPoints = number(rt, "armor.maxHitPoints", number(rt, "armor.hitPoints", 0));
			}
			const auto make = [&](const char* event) -> sound::ISound* {
				const std::string name = "S_" + m_interior.pco + event;
				sound::ISoundTemplate* st = m_manager.hasSound(name) ? m_manager.soundTemplate(name) : nullptr;
				sound::ISound* sound = st ? st->createSound() : nullptr;
				if (sound) {
					sound->setVolumeEnvelope(nullptr, &m_interior.speedInput);
				}
				return sound;
			};
			m_interior.ambient = make("_Ambient");
			m_interior.damaged = make("_AmbientDamaged");
		}
	}
	m_listenerState.seat = m_interior.pco;
	m_listenerState.listenerObstruction = vehicle ? m_interior.obstruction : 0;
	if (!vehicle) {
		m_listenerState.ambient.clear();
		m_manager.setInteriorAmbient(nullptr, 0);
		return;
	}
	m_interior.speedInput = std::clamp((std::sqrt(physics::lengthSquared(me.velocity)) - 0.1f) * 0.033333335f, 0.0f, 1.0f);
	bool damaged = false;
	if (m_interior.maxHitPoints > 0 && m_manager.hasSound("S_" + m_interior.pco + "_AmbientDamaged")) {
		const float hp = m_round.hitPoints(vehicle);
		damaged = hp >= 0 && hp < m_interior.maxHitPoints * m_interior.damagedLimit;
	}
	const std::string event = damaged ? "_AmbientDamaged" : "_Ambient";
	m_listenerState.ambient = m_manager.hasSound("S_" + m_interior.pco + event) ? "S_" + m_interior.pco + event : "";
	m_manager.setInteriorAmbient(damaged ? m_interior.damaged : m_interior.ambient, m_interior.obstruction);
}

// The soldier HUD's sounds (the vehicleHud object part, vtable
// 0x009890c0, from the HUD update 0x007a3180), the local player on foot:
// panting (slot 0x68, 0x00657590) on at no stamina left (soldier slot
// 0x108), off from 0.2 on (between, as it was), vehicleHud.pantingSound
// played and stopped by name (manager slots 0x2c / 0x34); the injury
// (slot 0x70, 0x006575f0, from 0x007a0f80): vehicleHud.injurySound once
// when the armour's fraction falls (not to 0) and the 2 s timer has run
// out; at 0 the timer is off (-1).
void GameAudio::soldierHud(const game::GameWorld::PlayerInfo& me, bool alive)
{
	const world::Object* soldier = alive && !me.vehicle ? m_objects.find(me.soldier) : nullptr;
	const world::ObjectTemplate* t = soldier ? soldier->objectTemplate() : nullptr;
	if (m_injuryTimer >= 0) {
		m_injuryTimer -= 1.0f / 30;
	}
	if (!t) {
		m_lastHealth = -1;
		if (!alive) {
			m_injuryTimer = -1;
		}
		return;
	}
	const std::string& panting = setting(*t, "vehicleHud.pantingSound");
	if (me.stamina > 0) {
		if (me.stamina >= 0.2f && m_panting) {
			m_manager.stopSound(m_pantingSound);
			m_panting = false;
		}
	} else if (!m_panting && !panting.empty()) {
		m_pantingSound = panting;
		m_manager.playSound(panting);
		m_panting = true;
	}
	m_listenerState.panting = m_panting;
	const float max = number(*t, "armor.maxHitPoints", number(*t, "armor.hitPoints", 0));
	const float hp = m_round.hitPoints(me.soldier);
	if (max <= 0 || hp < 0) {
		return;
	}
	const float f = std::clamp(hp / max, 0.0f, 1.0f);
	if (m_lastHealth >= 0 && f < m_lastHealth && f != 0 && m_injuryTimer <= 0) {
		const std::string& injury = setting(*t, "vehicleHud.injurySound");
		if (!injury.empty()) {
			m_manager.playSound(injury);
			++m_listenerState.injuries;
			m_injuryTimer = 2.0f;
		}
	}
	if (f == 0 && m_injuryTimer >= 0) {
		m_injuryTimer = -1;
	}
	m_lastHealth = f;
}

// The material radio (the soldier's collision handler 0x00586a90, on the
// server): a soldier touching a material with triggerRadio looks up its
// cells' radio messages (manager slot 0x78, 0x0060a730, each at most once
// in its delay, 0x0060c5e0): touching an object of its own team (a
// PlayerControlObject's root, interface 0xc4c5, its team slot 0x9c)
// index 0 for both; otherwise index 1 to its own team and index 2 to the
// other (TCServerTeamMessage, game slot 0x5c). The shipped data: the
// Titans' floors (veh_titan_armor, veh_titan_radio_1..9) with Human_body,
// index 2, "AUTO_TITAN_BB_enemydetecteda_inside" and the like, 30 s or
// 100 s. The rewrite's contact keeps no object: the Titan whose box holds
// the soldier is the one touched.
void GameAudio::materialRadio()
{
	const physics::MaterialManager* materials = m_round.materials();
	if (!materials || --m_materialScanIn > 0) {
		return;
	}
	m_materialScanIn = 3;
	std::vector<game::GameWorld::VehicleInfo> titans;
	bool listed = false;
	const double now = m_round.roundTime();
	for (const auto& p : m_round.playerList()) {
		if (!p.alive || p.vehicle) {
			continue;
		}
		const uint32_t touched = m_round.contactMaterial(p.id);
		const physics::Material* material = materials->find(touched);
		if (!material || !material->triggerRadio) {
			continue;
		}
		if (!listed) {
			listed = true;
			for (const auto& v : m_round.vehicleList()) {
				if (m_round.isTitan(v.object)) {
					titans.push_back(v);
				}
			}
		}
		bool friendly = false;
		for (const auto& titan : titans) {
			if (insideBox(titan, p.position + physics::Vec3 {0, 1.0f, 0})) {
				const world::Object* object = m_objects.find(titan.object);
				friendly = object && object->team() == p.team;
				break;
			}
		}
		const int own = friendly ? 0 : 1, other = friendly ? 0 : 2;
		for (const auto& [index, team] : {std::pair<int, int> {own, p.team}, {other, p.team == 1 ? 2 : 1}}) {
			const physics::MaterialCell::RadioMessage* radio = materials->radioMessage(touched, kSoldierBodyMaterial, index);
			if (!radio || (team != 1 && team != 2)) {
				continue;
			}
			double& next = m_materialRadioNext[{radio, index}];
			if (now < next) {
				continue;
			}
			next = now + radio->delay;
			m_round.radioMessage(-1, 13, radio->message, team);
			++materialRadios;
		}
	}
}

GameAudio::GameAudio(game::GameWorld& round, world::ObjectManager& objects, world::ObjectTemplateManager& templates,
	sound::SoundManager& manager, sound::GameSounds& sounds, sound::VoiceMessages* voices)
	: m_round(round), m_objects(objects), m_templates(templates), m_manager(manager), m_sounds(sounds), m_voices(voices)
{
}

int GameAudio::localPlayer() const { return m_hud ? m_hud->localPlayer() : m_round.localPlayer(); }

bool GameAudio::playerInfo(int player, game::GameWorld::PlayerInfo& out) const
{
	return m_hud ? m_hud->playerInfo(player, out) : m_round.playerInfo(player, out);
}

void GameAudio::effectSound(const std::string& name, const sound::Vec3& position) { m_sounds.play(name, position); }

void GameAudio::effectLoop(uint64_t key, const std::string& name, const sound::Vec3& position, float intensity)
{
	m_sounds.loop(ownerKey(3, key), name, position, {}, intensity);
}

void GameAudio::tick(const Camera& given)
{
	Camera camera = given;
	++m_tick;
	listener(camera);
	m_manager.setListener(camera.position, camera.forward, camera.up, camera.velocity, m_listenerState.insideTitan);
	autoRadio();
	materialRadio();
	for (const auto& sound : m_round.takeSounds()) {
		m_sounds.play(sound.sound, toSound(sound.position));
	}
	weapons(camera);
	engines();
	// The vehicles' rolling-surface loops (GameWorld::rollingSounds: the
	// PCO's contact sound object, 0x005c65d0 / 0x005c6510), at the vehicle
	// (its position provider, vtable 0x0096cf7c), envelopes on the eased
	// speed.
	for (const auto& rolling : m_round.rollingSounds()) {
		m_sounds.loop(ownerKey(4, rolling.vehicle), rolling.sound, toSound(rolling.position), toSound(rolling.velocity),
			rolling.input, rolling.vehicle);
	}
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

// The weapons' sound components (0x005dbdc0 fire, 0x005dbfe0 reload,
// 0x005dc1d0 deploy). An automatic weapon's looping fire sound stops
// (0x005db6c0, the component's slot 12) when its trigger is let go
// (0x005c1010), it overheats (0x005c3cb0) or can't fire on (0x005c4500,
// 0x005c58a0): the rewrite stops it when the player lets go, reloads, or
// no round has left for longer than its fire interval.
void GameAudio::weapons(const Camera& camera)
{
	const int local = m_round.localPlayer();
	std::map<int, game::GameWorld::PlayerInfo> infos;
	const auto info = [&](int player) -> const game::GameWorld::PlayerInfo* {
		auto it = infos.find(player);
		if (it == infos.end()) {
			game::GameWorld::PlayerInfo p;
			if (!m_round.playerInfo(player, p)) {
				return nullptr;
			}
			it = infos.emplace(player, p).first;
		}
		return &it->second;
	};
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
			: event.kind == Kind::TriggerClick                            ? sound::GameSounds::WeaponEvent::TriggerClick
																		  : sound::GameSounds::WeaponEvent::Deploy;
		const uint64_t user = (static_cast<uint64_t>(static_cast<uint32_t>(event.player)) << 32) | event.object;
		sound::GameSounds::FireInfo fire;
		if (event.kind == Kind::Fire) {
			const game::GameWorld::PlayerInfo* p = event.player >= 0 ? info(event.player) : nullptr;
			// The fire component's mode (slot 0xdc): the hand weapon's, a seat
			// gun's automatic (the rewrite's seats fire while held).
			fire.mode = p && !p->vehicle ? p->fireMode : 2;
			const int burst = weapon ? std::max(1, static_cast<int>(number(*weapon, "fire.burstSize", 3))) : 1;
			fire.burstRounds = fire.mode == 0 ? 1 : fire.mode == 1 ? burst : std::max(1, p ? p->roundsInMag : 1);
			fire.held = false;
			fire.roundsPerMinute = weapon ? static_cast<int>(number(*weapon, "fire.roundsPerMinute", 600)) : 600;
			Trigger& trigger = m_triggers[user];
			trigger.firing = true;
			trigger.lastRound = m_tick;
			trigger.interval = 2 + (fire.roundsPerMinute > 0 ? static_cast<int>(std::ceil(1800.0f / fire.roundsPerMinute)) : 0);
		} else if (event.kind == Kind::Reload) {
			m_sounds.releaseTrigger(user);
			m_triggers.erase(user);
		}
		m_sounds.weapon(kind, event.weapon, firstPerson, toSound(event.position), muteOnFire, user, event.object, &fire);
	}
	for (auto it = m_triggers.begin(); it != m_triggers.end();) {
		const int player = static_cast<int>(static_cast<uint32_t>(it->first >> 32));
		const game::GameWorld::PlayerInfo* p = info(player);
		const bool held = p && p->alive && p->firing;
		if (!held || m_tick - it->second.lastRound > it->second.interval) {
			m_sounds.releaseTrigger(it->first);
			it = m_triggers.erase(it);
		} else {
			++it;
		}
	}
}

// The new-style engine audio's step (0x007fe740 with 0x007fe570 and the
// sway 0x007fe430; audio.newStyleAudio, Engine template +0x7b4, set on the
// newCar2 APCs and FAVs). The throttle blend T (+0x78) moves towards
// on x clutch by at most 0.2 an update; Rpm1 plays at 1 - T and Rpm2 at T
// of their volume (0x007fe010). One input for Idle, Rpm1 and Rpm2:
// clamp(F x 0.9090909, 0, 1), F (+0x84) following |rpm| + jitter + sway
// through a critically damped smoother (0x006afeb0, engine-sound-smooth-
// time 0.1, a 30th of a second). The jitter (+0x80), while on and
// clutched, is drawn when its countdown (+0x7c) runs out: uniform in
// [-0.3a, a], a = engine-audio-material-tweak (10) x 0.1 x 6.5 / (5|rpm| +
// 1) x 0.06, the next in round((speed / 30 x 23 + 2) x uniform) updates.
// A gear change (gear at least 1) plays Load and starts the sway for
// tFirst + (g - 1) x 0.2 x (tLast - tFirst) seconds (engine-audio-sway-
// time-first / -last, 0.7 / 0.7): sin((fFirst + (fLast - fFirst) g) x t x
// 0.85) x (aFirst + (aLast - aFirst) g) x the time left, halved after a
// downshift, else times on (engine-audio-sway-frequency-* 80 / 35,
// -amplitude-* 0.05 / 0.01). The clock's period (0x007fe430's _CIfmod)
// isn't read: the rewrite uses the round's seconds. The ground-contact
// int that adds 0.3 to the jitter (the object at engine +0x298, +0x188)
// is taken as none (-2).
void GameAudio::stepNewStyle(NewStyleEngine& e, bool on, float clutch, float rpm, int gear, float speed, double clock)
{
	const Vars& vars = Vars::global();
	const float r = std::clamp(rpm, -1.0f, 1.0f);
	const float want = (on ? 1.0f : 0.0f) * clutch;
	e.throttleBlend += std::clamp(want - e.throttleBlend, -0.2f, 0.2f);
	e.rpm1Scale = std::clamp(1.0f - e.throttleBlend, 0.0f, 1.0f);
	e.rpm2Scale = std::clamp(e.throttleBlend, 0.0f, 1.0f);
	const auto uniform = [&e] {
		e.random = e.random * 1103515245u + 12345u;
		return static_cast<float>((e.random >> 8) & 0xffffff) / 16777216.0f;
	};
	if (!on || clutch == 0) {
		e.jitter = 0;
	} else if (e.jitterCountdown < 1) {
		const float a = vars.number("engine-audio-material-tweak", 10) * 0.1f * 6.5f / (5 * std::fabs(r) + 1) * 0.06f;
		e.jitter = -0.3f * a + uniform() * 1.3f * a;
		e.jitterCountdown = static_cast<int>(std::lround((speed / 30 * 23 + 2) * uniform()));
	}
	--e.jitterCountdown;
	// The sway.
	const int g = std::max(gear, 1);
	e.shifted = false;
	if (g != e.gear) {
		e.lastGear = e.gear < 0 ? g : e.gear;
		e.gear = g;
		const float first = vars.number("engine-audio-sway-time-first", 0.7f);
		const float last = vars.number("engine-audio-sway-time-last", 0.7f);
		e.swayLeft = first + (g - 1) * 0.2f * (last - first);
		e.shifted = true;
	} else {
		e.swayLeft -= 1.0f / 30;
	}
	float sway = 0;
	if (e.swayLeft >= 0) {
		const float a0 = vars.number("engine-audio-sway-amplitude-first", 0.05f);
		const float a1 = vars.number("engine-audio-sway-amplitude-last", 0.01f);
		const float f0 = vars.number("engine-audio-sway-frequency-first", 80);
		const float f1 = vars.number("engine-audio-sway-frequency-last", 35);
		float amplitude = (a0 + (a1 - a0) * g) * e.swayLeft;
		amplitude *= g < e.lastGear ? 0.5f : (on ? 1.0f : 0.0f);
		sway = std::sin((f0 + (f1 - f0) * g) * static_cast<float>(clock) * 0.85f) * amplitude;
	}
	const float target = sway + e.jitter + std::fabs(r);
	// The critically damped smoother (0x006afeb0).
	if (std::fabs(target - e.smoothed) < 0.001f) {
		e.smoothed = target;
		e.smoothedSpeed = 0;
	} else {
		const float omega = 2.0f / vars.number("engine-sound-smooth-time", 0.1f);
		const float x = omega / 30;
		const float decay = 1.0f / (1 + x + 0.48f * x * x + 0.235f * x * x * x);
		const float change = e.smoothed - target;
		const float temp = (e.smoothedSpeed + omega * change) / 30;
		e.smoothedSpeed = (e.smoothedSpeed - omega * temp) * decay;
		e.smoothed = target + (change + temp) * decay;
	}
	e.input = std::clamp(e.smoothed * 0.9090909f, 0.0f, 1.0f);
	if (std::fabs(e.input) < 1e-4f) {
		e.input = 0;
	}
}

void GameAudio::engines()
{
	std::set<uint32_t> aboard;
	std::map<uint32_t, physics::Vec3> speeds;
	for (const auto& player : m_round.playerList()) {
		if (player.alive && player.vehicle) {
			aboard.insert(player.vehicle);
			speeds[player.vehicle] = player.velocity;
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
			const uint64_t owner = ownerKey(1, (static_cast<uint64_t>(id) << 8) | index);
			auto style = m_newStyleEngines.find(part.name);
			if (style == m_newStyleEngines.end()) {
				const world::ObjectTemplate* t = m_templates.find(part.name);
				style = m_newStyleEngines.emplace(part.name, t && setting(*t, "audio.newStyleAudio") == "1").first;
			}
			if (style->second) {
				NewStyleEngine& e = m_newStyle[owner];
				const physics::Vec3& v = speeds[id];
				stepNewStyle(e, std::fabs(vehicle->input().throttle) > 0, vehicle->clutch(), rpm, vehicle->gear(),
					std::sqrt(physics::lengthSquared(v)), m_round.roundTime());
				if (e.shifted) {
					m_sounds.play("S_" + part.name + "_Load", at, id);
				}
				m_sounds.loop(owner, "S_" + part.name + "_Idle", at, {}, e.input, id);
				m_sounds.loop(owner, "S_" + part.name + "_Rpm1", at, {}, e.input, id, &e.rpm1Scale);
				m_sounds.loop(owner, "S_" + part.name + "_Rpm2", at, {}, e.input, id, &e.rpm2Scale);
				continue;
			}
			// The old style: |rpm| x 0.8333 for all three.
			const float input = std::fabs(rpm * 0.8333333f);
			for (const char* loop : {"_Idle", "_Rpm1", "_Rpm2"}) {
				m_sounds.loop(owner, "S_" + part.name + loop, at, {}, input, id);
			}
		}
	}
}

// The round's Sound child, created and played with it (its loop; the
// template's own child, as the S_<name>_Looping most carry, a few named
// otherwise: static_ap_railgun_projectile's
// S_static_ap_railgun_projectile_projectile_Looping), else that name.
const std::string& GameAudio::loopingSound(const void*, const std::string& name)
{
	auto it = m_looping.find(name);
	if (it == m_looping.end()) {
		std::string loop;
		if (const world::ObjectTemplate* round = m_templates.find(name)) {
			for (const world::ChildTemplate& child : round->children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				if (c && c->typeName() == "Sound") {
					loop = child.name;
					break;
				}
			}
		}
		if (loop.empty() && m_manager.soundTemplate("S_" + name + "_Looping")) {
			loop = "S_" + name + "_Looping";
		}
		it = m_looping.emplace(name, loop).first;
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
	const int local = localPlayer();
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
	if (!playerInfo(local, me)) {
		return;
	}
	for (const auto& message : heard) {
		++radioHeard;
		sound::VoiceMessages::Heard in;
		in.message = message.voice;
		in.listenerTeam = me.team;
		in.listener = toSound(me.position);
		game::GameWorld::PlayerInfo sender;
		if (message.sender >= 0 && playerInfo(message.sender, sender)) {
			in.hasSender = true;
			in.senderIsListener = message.sender == local;
			in.commander = sender.commander;
			in.squadLeader = sender.squadLeader;
			in.onFoot = sender.alive && sender.vehicle == 0;
			in.senderFilter = m_round.radioFilter(sender.id);
			in.sender.id = sender.id;
			in.sender.team = sender.team;
			in.sender.position = toSound(sender.position) + sound::Vec3 {0, 1.6f, 0};
		} else if (message.sender < 0 && message.senderTeam != 0) {
			// An auto controller's player (GameWorld::autoControllerReport):
			// neither commander nor squad leader ("grunt"), his controlled
			// object the sentry (not a soldier: the radio, not the local
			// voice), its soundFilter (0x0042e0c0).
			const world::Object* object = m_objects.find(message.senderObject);
			in.hasSender = true;
			in.senderIsListener = false;
			if (object && object->objectTemplate()) {
				in.senderFilter = setting(*object->objectTemplate(), "soundFilter");
			}
			in.sender.team = message.senderTeam;
		}
		// The sender's soldier plays the message's emote (0x0042e0c0, before
		// the voice: the sender's controlled object a soldier, class 0x9493).
		if (onEmote && in.hasSender && in.onFoot) {
			const std::string emote = m_voices->emote(m_voices->message(message.voice));
			if (!emote.empty()) {
				onEmote(message.sender, emote);
			}
		}
		const sound::VoiceMessages::Result result = m_voices->heard(in);
		if (result.played != sound::VoiceMessages::Played::None) {
			++voiceOvers;
		}
		if (onRadioText && !result.text.empty()) {
			onRadioText(message.sender, message.channel, result.text);
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
	const int local = localPlayer();
	game::GameWorld::PlayerInfo me;
	const int localTeam = playerInfo(local, me) ? me.team : 0;
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
			// A silo's index, 1..5 as the voice table has them: its place
			// among the silos ordered by control point id, the order the
			// map numbers them in (the levels' ids run 202, 503..506).
			int silo = 0;
			const auto silos = m_round.missileSilos();
			for (size_t i = 0; i < silos.size(); ++i) {
				if (silos[i].object != it->first) {
					continue;
				}
				silo = 1;
				for (const auto& other : silos) {
					if (other.controlPoint < silos[i].controlPoint) {
						++silo;
					}
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
