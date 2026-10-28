// "Sound.*" console methods and the "Sound" template's "ObjectTemplate.*" properties.
#pragma once

namespace dice::hfe {
class Console;
}

namespace dice::hfe::sound {

class SoundManager;
class VoiceMessages;

void registerSoundConsoleMethods(Console& console, SoundManager& sounds);
// "GameLogic.messages.*" (Sound/VoiceMessages.con): the voice-message
// table's addMessage, addRadioVoice, addRadioFilter, addHelpMessage,
// addEmoteTrigger and setVolumes.
void registerVoiceMessageConsoleMethods(Console& console, VoiceMessages& messages);

} // namespace dice::hfe::sound
