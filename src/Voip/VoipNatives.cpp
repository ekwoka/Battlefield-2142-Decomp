#include "VoipNatives.h"

#include "UI/Avm1/Interpreter.h"
#include "UI/Player/Natives.h"
#include "VoiceChat.h"

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

void bindVoipNatives(ui::NativeRegistry& n, VoiceChat& chat)
{
	// voipInitialize: the options page opens the devices for its test.
	n.setFunction(kSound, "voipInitialize", [&chat](Interpreter&, const Args&) { return Value(chat.openDevice()); });
	const auto setting = [&n, &chat](const char* set, const char* get, auto member) {
		n.setFunction(kSound, set, [&chat, member](Interpreter& vm, const Args& args) {
			VoiceSettings s = chat.settings();
			using T = std::decay_t<decltype(s.*member)>;
			if constexpr (std::is_same_v<T, bool>) {
				s.*member = ui::avm1::toBoolean(arg(args, 0));
			} else {
				s.*member = static_cast<float>(vm.toNumber(arg(args, 0)));
			}
			chat.setSettings(s);
			return Value();
		});
		n.setFunction(kSound, get, [&chat, member](Interpreter&, const Args&) { return Value(chat.settings().*member); });
	};
	setting("voipSetEnable", "voipGetEnable", &VoiceSettings::enabled);
	setting("voipSetBoost", "voipGetBoost", &VoiceSettings::boost);
	setting("voipSetTransmitVolume", "voipGetTransmitVolume", &VoiceSettings::captureVolume);
	setting("voipSetRecieveVolume", "voipGetRecieveVolume", &VoiceSettings::playbackVolume);
	setting("voipSetThresholdVolume", "voipGetThresholdVolume", &VoiceSettings::captureThreshold);
	setting("voipSetUsePushToTalk", "voipGetUsePushToTalk", &VoiceSettings::pushToTalk);
	n.setFunction(kSound, "voipIsBoostEnabled", [&chat](Interpreter&, const Args&) { return Value(chat.settings().boost); });
	// The microphone test: what is said comes back through the codec.
	n.setFunction(kSound, "voipStartTest", [&chat](Interpreter&, const Args&) {
		chat.startTest();
		return Value();
	});
	n.setFunction(kSound, "voipStopTest", [&chat](Interpreter&, const Args&) {
		chat.stopTest();
		return Value();
	});
	n.setFunction(kSound, "voipSendingVoice", [&chat](Interpreter&, const Args&) { return Value(chat.sendingVoice()); });
	n.setFunction(kSound, "voipGetSendingVoiceVolume",
		[&chat](Interpreter&, const Args&) { return Value(static_cast<double>(chat.sendingVoiceVolume())); });
}

} // namespace dice::hfe::voip
