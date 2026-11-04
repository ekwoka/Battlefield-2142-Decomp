// The menu's voice natives (dice.bf2.Sound.voip*, SwiffPlayer's native
// table): the options page's voice settings and its microphone test.
// Replaces MenuBindings' stand-ins when called after it.
#pragma once

#include <functional>
#include <string>

namespace dice::hfe::ui {
class NativeRegistry;
}

namespace dice::hfe::voip {

class VoiceChat;

// The AudioSettings object the natives write through (SwiffPlayer's
// setters store each value in it as well as in the client): store("Voip
// Enabled", 1) for AudioSettings.VoipEnabled; read gives its value (or the
// fallback). Both may be empty (the client alone).
struct VoipSettingsStore {
	std::function<void(const std::string& name, double value)> store;
	std::function<double(const std::string& name, double fallback)> read;
};

// The natives call into `chat` (keep it alive while the menu runs).
void bindVoipNatives(ui::NativeRegistry& natives, VoiceChat& chat, VoipSettingsStore settings = {});

} // namespace dice::hfe::voip
