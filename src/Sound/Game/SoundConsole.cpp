#include "SoundConsole.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "SoundManager.h"
#include "SoundObjectTemplate.h"

namespace dice::hfe::sound {

namespace {

// Registers "ObjectTemplate.<name>" for Sound templates only: the lookup
// skips it unless the active template is a Sound.
template <class T>
void bindSoundProperty(Console& console, world::ObjectTemplateManager& templates, const char* name,
	std::function<T(SoundObjectTemplate&)> get, std::function<void(SoundObjectTemplate&, const T&)> set)
{
	const auto active = [&templates] { return dynamic_cast<SoundObjectTemplate*>(templates.active()); };
	ConsoleMethod method;
	method.name = std::string("ObjectTemplate.") + name;
	method.kind = ConsoleMethodKind::ReadWriteProperty;
	method.access = ConsoleAccess::Anyone;
	method.maxArgs = 1;
	method.isActive = [active] { return active() != nullptr; };
	method.invoke = [active, get, set](const std::vector<std::string>& args) -> std::string {
		SoundObjectTemplate& sound = *active();
		if (args.empty()) {
			return formatConsoleValue(get(sound));
		}
		set(sound, parseConsoleValue<T>(args[0]));
		return {};
	};
	console.registerMethod(std::move(method));
}

Vec3 toVec3(const ConsoleVec3& v) { return {v.x, v.y, v.z}; }
ConsoleVec3 toConsole(const Vec3* v) { return v ? ConsoleVec3 {v->x, v->y, v->z} : ConsoleVec3 {}; }

} // namespace

void registerSoundConsoleMethods(Console& console, SoundManager& sounds)
{
	const uint32_t access = ConsoleAccess::Anyone;

	// --- Sound ------------------------------------------------------------
	bindMethod(console, "Sound.initialize", access, [&sounds](int quality, std::string provider, int voices) {
		return sounds.initialize(quality, provider, voices);
	});
	// addMenuSound name file [volume [loop [interface]]]; the volume defaults to 1.
	console.registerMethod([&] {
		ConsoleMethod method;
		method.name = "Sound.addMenuSound";
		method.access = access;
		method.minArgs = 2;
		method.maxArgs = 5;
		method.invoke = [&sounds](const std::vector<std::string>& args) {
			const float volume = args.size() > 2 ? parseConsoleValue<float>(args[2]) : 1.0f;
			const bool loop = args.size() > 3 && parseConsoleValue<bool>(args[3]);
			const bool interface = args.size() > 4 && parseConsoleValue<bool>(args[4]);
			sounds.addMenuSound(args[0], args[1], volume, loop, interface);
			return std::string();
		};
		return method;
	}());
	bindMethod(console, "Sound.addSound", access, [&sounds](std::string name) { sounds.addSound(name); });
	bindMethod(console, "Sound.playSound", access, [&sounds](std::string name) { sounds.playSound(name); });
	bindMethod(console, "Sound.stopSound", access, [&sounds](std::string name) { sounds.stopSound(name); });
	bindMethod(console, "Sound.setProperty", access,
		[&sounds](std::string name, float value) { sounds.setProperty(name, value); });
	bindMethod(console, "Sound.getProperty", access, [&sounds](std::string name) { return sounds.property(name); });
	bindMethod(console, "Sound.setReverb", access, [&sounds](std::string path) { sounds.setReverb(path); });
	bindMethod(console, "Sound.tinnitusSetup", access,
		[&sounds](std::string files, float attack, float sustain, float release, float level) {
			sounds.setupTinnitus(files, attack, sustain, release, level);
		});
	bindProperty<float>(console, "Sound.masterVolume", access, [&sounds] { return sounds.masterVolume(); },
		[&sounds](const float& volume) { sounds.setMasterVolume(volume); });
	bindProperty<float>(console, "Sound.dopplerEffect", access, [&sounds] { return sounds.dopplerFactor(); },
		[&sounds](const float& factor) { sounds.setDopplerFactor(factor); });
	bindMethod(console, "Sound.setVolumeForGroup", access,
		[&sounds](int group, float volume) { sounds.setVolumeForGroup(group, volume); });
	bindMethod(console, "Sound.getVolumeForGroup", access, [&sounds](int group) { return sounds.volumeForGroup(group); });
	bindMethod(console, "Sound.loadMusic", access, [&sounds](std::string path) { return sounds.loadMusic(path); });
	bindMethod(console, "Sound.playMusic", access, [&sounds] { return sounds.playMusic(); });
	bindMethod(console, "Sound.stopMusic", access, [&sounds](float fade) { return sounds.stopMusic(fade); });
	bindMethod(console, "Sound.tweakTemplate", access,
		[&sounds](std::string name, float a, float b, float c, float d, float e, float f) {
			sounds.tweakTemplate(name, {a, b, c, d, e, f});
		});
	bindMethod(console, "Sound.getAllSoundTemplates", access, [&sounds] { return sounds.allSoundTemplates(); });

	// --- ObjectTemplate properties of Sound templates ------------------------
	world::ObjectTemplateManager& templates = sounds.templates();
	using S = SoundObjectTemplate;
	bindSoundProperty<std::string>(console, templates, "soundFilename", [](S& s) { return s.soundFilenames(); },
		[](S& s, const std::string& v) { s.setSoundFilenames(v); });
	bindSoundProperty<std::string>(console, templates, "volumeEnvelope", [](S& s) { return s.volumeEnvelope(); },
		[](S& s, const std::string& v) { s.setVolumeEnvelope(v); });
	bindSoundProperty<std::string>(console, templates, "pitchEnvelope", [](S& s) { return s.pitchEnvelope(); },
		[](S& s, const std::string& v) { s.setPitchEnvelope(v); });
	bindSoundProperty<float>(console, templates, "volume", [](S& s) { return s.volume(); },
		[](S& s, const float& v) { s.setVolume(v); });
	bindSoundProperty<float>(console, templates, "pitch", [](S& s) { return s.pitch(); },
		[](S& s, const float& v) { s.setPitch(v); });
	bindSoundProperty<float>(console, templates, "minDistance", [](S& s) { return s.minDistance(); },
		[](S& s, const float& v) { s.setMinDistance(v); });
	bindSoundProperty<float>(console, templates, "halfVolumeDistance", [](S& s) { return s.halfVolumeDistance(); },
		[](S& s, const float& v) { s.setHalfVolumeDistance(v); });
	bindSoundProperty<float>(console, templates, "reverbLevel", [](S& s) { return s.reverbLevel(); },
		[](S& s, const float& v) { s.setReverbLevel(v); });
	bindSoundProperty<float>(console, templates, "soundRadius", [](S& s) { return s.soundRadius(); },
		[](S& s, const float& v) { s.setSoundRadius(v); });
	bindSoundProperty<bool>(console, templates, "isTitanAmbient", [](S& s) { return s.isTitanAmbient(); },
		[](S& s, const bool& v) { s.setTitanAmbient(v); });
	bindSoundProperty<float>(console, templates, "globalEffectLevel", [](S& s) { return s.globalEffectLevel; },
		[](S& s, const float& v) { s.globalEffectLevel = v; });

	// The rest go straight to the engine template.
	const auto engine = [](S& s) { return s.engineTemplate(); };
	bindSoundProperty<int>(console, templates, "loopCount",
		[engine](S& s) { return engine(s) ? engine(s)->loopCount() : 0; },
		[engine](S& s, const int& v) { if (engine(s)) engine(s)->setLoopCount(v); });
	bindSoundProperty<int>(console, templates, "lowSamples",
		[engine](S& s) { return engine(s) ? engine(s)->sampleCount(2) : 0; },
		[engine](S& s, const int& v) { if (engine(s)) engine(s)->setSampleCount(2, v); });
	bindSoundProperty<int>(console, templates, "mediumSamples",
		[engine](S& s) { return engine(s) ? engine(s)->sampleCount(1) : 0; },
		[engine](S& s, const int& v) { if (engine(s)) engine(s)->setSampleCount(1, v); });
	bindSoundProperty<int>(console, templates, "randomMode",
		[engine](S& s) { return engine(s) ? static_cast<int>(engine(s)->randomMode()) : 0; },
		[engine](S& s, const int& v) { if (engine(s)) engine(s)->setRandomMode(static_cast<unsigned char>(v)); });
	bindSoundProperty<bool>(console, templates, "is3dSound",
		[engine](S& s) { return engine(s) && engine(s)->is3dSound(); },
		[engine](S& s, const bool& v) { if (engine(s)) engine(s)->setIs3dSound(v); });
	bindSoundProperty<int>(console, templates, "stopType",
		[engine](S& s) { return engine(s) ? engine(s)->stopType() : 0; },
		[engine](S& s, const int& v) { if (engine(s)) engine(s)->setStopType(v); });
	bindSoundProperty<int>(console, templates, "volumeGroup",
		[engine](S& s) { return engine(s) ? engine(s)->volumeGroup() : 0; },
		[engine](S& s, const int& v) { if (engine(s)) engine(s)->setVolumeGroup(v); });
	bindSoundProperty<ConsoleVec3>(console, templates, "position",
		[engine](S& s) { return engine(s) ? toConsole(engine(s)->position()) : ConsoleVec3 {}; },
		[engine](S& s, const ConsoleVec3& v) {
			if (engine(s)) {
				const Vec3 p = toVec3(v);
				engine(s)->setPosition(&p);
			}
		});
	bindSoundProperty<ConsoleVec3>(console, templates, "coneDirection",
		[engine](S& s) { return engine(s) ? toConsole(engine(s)->direction()) : ConsoleVec3 {}; },
		[engine](S& s, const ConsoleVec3& v) {
			if (engine(s)) {
				const Vec3 d = toVec3(v);
				engine(s)->setDirection(&d);
			}
		});
	bindSoundProperty<float>(console, templates, "coneInnerAngle",
		[engine](S& s) { return engine(s) ? engine(s)->coneInnerAngle() : 0.0f; },
		[engine](S& s, const float& v) { if (engine(s)) engine(s)->setConeInnerAngle(v); });
	bindSoundProperty<float>(console, templates, "coneOuterAngle",
		[engine](S& s) { return engine(s) ? engine(s)->coneOuterAngle() : 0.0f; },
		[engine](S& s, const float& v) { if (engine(s)) engine(s)->setConeOuterAngle(v); });
	bindSoundProperty<float>(console, templates, "coneOuterVolume",
		[engine](S& s) { return engine(s) ? engine(s)->coneOuterVolume() : 0.0f; },
		[engine](S& s, const float& v) { if (engine(s)) engine(s)->setConeOuterVolume(v); });
}

} // namespace dice::hfe::sound
