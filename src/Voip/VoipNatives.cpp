#include "VoipNatives.h"

#include "UI/Avm1/Interpreter.h"
#include "UI/Player/Natives.h"
#include "VoiceChat.h"

#include <memory>

namespace dice::hfe::voip {

namespace {

using ui::avm1::Interpreter;
using ui::avm1::Value;
using Args = std::vector<Value>;

constexpr const char* kSound = "dice.bf2.Sound";

Value arg(const Args& args, size_t i)
{
	return i < args.size() ? args[i] : Value();
}

} // namespace

// SwiffPlayer's natives (registered at 0x10059xxx..0x1005axxx). Each one
// works only after voipInitialize (0x10059fa0, the flag DAT_1012c004):
// it opens the devices, then loads the AudioSettings object's values into
// the client (the playback volume, slot 0x40 → client 0x1c; the capture
// volume 0x48 → 0x20; the boost 0x58 → 0xcc; the threshold 0x50, kept in
// DAT_10118cc4 → 0x14). Every setter writes the client and the
// AudioSettings object alike, so the values stay with the profile:
// voipSetEnable (0x1005a240) AudioSettings slot 0x3c and the client's
// disable (slot 0x98); voipSetRecieveVolume (0x1005a850) 0x44;
// voipSetTransmitVolume (0x1005a6e0) 0x4c; voipSetThresholdVolume
// (0x1005a9c0) 0x54, only remembered during the microphone test;
// voipSetBoost (0x1005aac0) 0x5c. voipGetEnable (0x1005a350) reads the
// AudioSettings object (slot 0x38), 0 before voipInitialize; the other
// getters read the client. The test (voipStartTest 0x1005a470 /
// voipStopTest 0x1005a5b0) runs with the threshold at 0 and puts the
// remembered one back (client and AudioSettings) when it stops.
void bindVoipNatives(ui::NativeRegistry& n, VoiceChat& chat, VoipSettingsStore store)
{
	struct State {
		bool initialized = false; // DAT_1012c004
		bool testing = false;     // DAT_1012c005
		float threshold = 0.1f;   // DAT_10118cc4
	};
	const auto state = std::make_shared<State>();
	const auto save = [store](const char* name, double value) {
		if (store.store) {
			store.store(std::string("AudioSettings.") + name, value);
		}
	};
	const auto load = [store](const char* name, double fallback) {
		return store.read ? store.read(std::string("AudioSettings.") + name, fallback) : fallback;
	};
	n.setFunction(kSound, "voipInitialize", [&chat, state, load](Interpreter&, const Args&) {
		if (!chat.openDevice()) {
			return Value(false);
		}
		VoiceSettings s = chat.settings();
		s.playbackVolume = static_cast<float>(load("VoipPlaybackVolume", s.playbackVolume));
		s.captureVolume = static_cast<float>(load("VoipCaptureVolume", s.captureVolume));
		s.boost = load("VoipBoostEnabled", s.boost) != 0;
		state->threshold = static_cast<float>(load("VoipCaptureThreshold", s.captureThreshold));
		s.captureThreshold = state->testing ? 0.0f : state->threshold;
		chat.setSettings(s);
		state->initialized = true;
		return Value(true);
	});
	// A setter: the client's value and the AudioSettings object's.
	const auto setting = [&n, &chat, state, save](const char* set, const char* get, const char* name, auto member) {
		n.setFunction(kSound, set, [&chat, state, save, name, member](Interpreter& vm, const Args& args) {
			if (!state->initialized) {
				return Value();
			}
			VoiceSettings s = chat.settings();
			using T = std::decay_t<decltype(s.*member)>;
			if constexpr (std::is_same_v<T, bool>) {
				s.*member = ui::avm1::toBoolean(arg(args, 0));
				save(name, s.*member ? 1 : 0);
			} else {
				s.*member = static_cast<float>(vm.toNumber(arg(args, 0)));
				save(name, s.*member);
			}
			chat.setSettings(s);
			return Value(true);
		});
		n.setFunction(kSound, get, [&chat, state, member](Interpreter&, const Args&) {
			return state->initialized ? Value(chat.settings().*member) : Value();
		});
	};
	setting("voipSetBoost", "voipGetBoost", "VoipBoostEnabled", &VoiceSettings::boost);
	setting("voipSetTransmitVolume", "voipGetTransmitVolume", "VoipCaptureVolume", &VoiceSettings::captureVolume);
	setting("voipSetRecieveVolume", "voipGetRecieveVolume", "VoipPlaybackVolume", &VoiceSettings::playbackVolume);
	setting("voipSetEnable", "voipGetEnable", "VoipEnabled", &VoiceSettings::enabled);
	n.setFunction(kSound, "voipGetEnable", [&chat, state, load](Interpreter&, const Args&) {
		return Value(state->initialized && load("VoipEnabled", chat.settings().enabled) != 0);
	});
	n.setFunction(kSound, "voipSetThresholdVolume", [&chat, state, save](Interpreter& vm, const Args& args) {
		if (!state->initialized) {
			return Value();
		}
		state->threshold = static_cast<float>(vm.toNumber(arg(args, 0)));
		if (!state->testing) {
			VoiceSettings s = chat.settings();
			s.captureThreshold = state->threshold;
			chat.setSettings(s);
			save("VoipCaptureThreshold", state->threshold);
		}
		return Value();
	});
	n.setFunction(kSound, "voipGetThresholdVolume", [&chat, state](Interpreter&, const Args&) {
		if (!state->initialized) {
			return Value();
		}
		return Value(static_cast<double>(state->testing ? state->threshold : chat.settings().captureThreshold));
	});
	// Push-to-talk (0x1005acb0 / 0x1005adf0) is the client's alone.
	n.setFunction(kSound, "voipSetUsePushToTalk", [&chat](Interpreter&, const Args& args) {
		VoiceSettings s = chat.settings();
		s.pushToTalk = ui::avm1::toBoolean(arg(args, 0));
		chat.setSettings(s);
		return Value();
	});
	n.setFunction(kSound, "voipGetUsePushToTalk", [&chat](Interpreter&, const Args&) { return Value(chat.settings().pushToTalk); });
	n.setFunction(kSound, "voipIsBoostEnabled", [&chat](Interpreter&, const Args&) { return Value(chat.settings().boost); });
	// The microphone test: what is said comes back through the codec, with
	// the threshold at 0.
	n.setFunction(kSound, "voipStartTest", [&chat, state](Interpreter&, const Args&) {
		if (!chat.openDevice()) {
			return Value(false);
		}
		state->testing = true;
		VoiceSettings s = chat.settings();
		s.captureThreshold = 0;
		chat.setSettings(s);
		chat.startTest();
		return Value();
	});
	n.setFunction(kSound, "voipStopTest", [&chat, state, save](Interpreter&, const Args&) {
		if (!state->testing) {
			return Value();
		}
		state->testing = false;
		chat.stopTest();
		VoiceSettings s = chat.settings();
		s.captureThreshold = state->threshold;
		chat.setSettings(s);
		save("VoipCaptureThreshold", state->threshold);
		return Value();
	});
	n.setFunction(kSound, "voipSendingVoice", [&chat](Interpreter&, const Args&) { return Value(chat.sendingVoice()); });
	n.setFunction(kSound, "voipGetSendingVoiceVolume",
		[&chat](Interpreter&, const Args&) { return Value(static_cast<double>(chat.sendingVoiceVolume())); });
}

} // namespace dice::hfe::voip
