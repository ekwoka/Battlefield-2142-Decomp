// The console methods that build animation systems (AnimationSystem.*,
// AnimationManager.*, AnimationBundle.*, AnimationTrigger.*,
// AnimationValueHolder.*), acting on the system being loaded.
#pragma once

#include "AnimationSystem.h"

namespace dice::hfe {
class Console;
}

namespace dice::hfe::anim {

void registerAnimationConsoleMethods(Console& console, AnimationSystems& systems);

} // namespace dice::hfe::anim
