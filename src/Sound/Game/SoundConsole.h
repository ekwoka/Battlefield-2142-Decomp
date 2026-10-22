// "Sound.*" console methods and the "Sound" template's "ObjectTemplate.*" properties.
#pragma once

namespace dice::hfe {
class Console;
}

namespace dice::hfe::sound {

class SoundManager;

void registerSoundConsoleMethods(Console& console, SoundManager& sounds);

} // namespace dice::hfe::sound
