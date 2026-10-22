// The menu's voice natives (dice.bf2.Sound.voip*, SwiffPlayer's native
// table): the options page's voice settings and its microphone test.
// Replaces MenuBindings' stand-ins when called after it.
#pragma once

namespace dice::hfe::ui {
class NativeRegistry;
}

namespace dice::hfe::voip {

class VoiceChat;

// The natives call into `chat` (keep it alive while the menu runs).
void bindVoipNatives(ui::NativeRegistry& natives, VoiceChat& chat);

} // namespace dice::hfe::voip
