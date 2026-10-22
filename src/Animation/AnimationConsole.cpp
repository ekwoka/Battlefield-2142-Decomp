#include "AnimationConsole.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

namespace dice::hfe::anim {

namespace {

constexpr uint32_t kAccess = ConsoleAccess::Anyone;

} // namespace

void registerAnimationConsoleMethods(Console& console, AnimationSystems& systems)
{
	AnimationSystems* s = &systems;
	const auto system = [s]() { return s->active(); };
	const auto clip = [s]() -> Clip* {
		AnimationSystemTemplate* t = s->active();
		return t && s->activeClip >= 0 ? &t->clips()[static_cast<size_t>(s->activeClip)] : nullptr;
	};
	const auto bundle = [s]() -> Bundle* {
		AnimationSystemTemplate* t = s->active();
		return t && s->activeBundle >= 0 ? &t->bundles()[static_cast<size_t>(s->activeBundle)] : nullptr;
	};
	const auto trigger = [s]() -> Trigger* {
		AnimationSystemTemplate* t = s->active();
		return t && s->activeTrigger >= 0 ? &t->triggers()[static_cast<size_t>(s->activeTrigger)] : nullptr;
	};

	// AnimationSystem.*
	bindMethod(console, "AnimationSystem.createAnimation", kAccess, [s, system](std::string path) {
		if (AnimationSystemTemplate* t = system()) {
			s->activeClip = static_cast<int>(t->addClip(path));
		}
	});
	bindMethod(console, "AnimationSystem.createBundle", kAccess, [s, system](std::string name) {
		if (AnimationSystemTemplate* t = system()) {
			s->activeBundle = static_cast<int>(t->addBundle(name));
		}
	});
	bindMethod(console, "AnimationSystem.createTrigger", kAccess, [s, system](std::string type, std::string name) {
		TriggerType parsed;
		AnimationSystemTemplate* t = system();
		if (t && parseTriggerType(type, parsed)) {
			s->activeTrigger = static_cast<int>(t->addTrigger(parsed, name));
		}
	});
	bindMethod(console, "AnimationSystem.createValueHolder", kAccess,
		[s](std::string name) { s->createValueHolder(name); });
	for (const char* name : {"cameraSpring.use", "cameraSpring.amplitude", "cameraSpring.zoomDamping",
			 "cameraSpring.stiffness", "cameraSpring.look", "cameraSpring.move", "cameraSpring.hit",
			 "cameraSpring.fire", "playbackModifier"}) {
		// Kept by the original for first-person cameras; not used yet.
		bindMethod(console, std::string("AnimationSystem.") + name, kAccess, [](std::string) {}, 0);
	}

	// AnimationManager.*: the clip created last.
	bindProperty<bool>(console, "AnimationManager.looping", kAccess,
		[clip] { return clip() ? clip()->looping : true; },
		[clip](const bool& v) {
			if (Clip* c = clip()) {
				c->looping = v;
			}
		});
	bindProperty<float>(console, "AnimationManager.length", kAccess,
		[clip, s] { return clip() ? s->clipLength(*clip()) : 0.0f; },
		[clip](const float& v) {
			if (Clip* c = clip()) {
				c->length = v;
			}
		});
	bindProperty<float>(console, "AnimationManager.fadeInTime", kAccess,
		[clip] { return clip() ? clip()->fadeInTime : 0.0f; },
		[clip](const float& v) {
			if (Clip* c = clip()) {
				c->fadeInTime = v;
			}
		});
	bindProperty<int>(console, "AnimationManager.noiceFreq", kAccess,
		[clip] { return clip() ? clip()->noiseFrequency : 0; },
		[clip](const int& v) {
			if (Clip* c = clip()) {
				c->noiseFrequency = v;
			}
		});
	bindProperty<float>(console, "AnimationManager.noiceAmplitude", kAccess,
		[clip] { return clip() ? clip()->noiseAmplitude : 0.0f; },
		[clip](const float& v) {
			if (Clip* c = clip()) {
				c->noiseAmplitude = v;
			}
		});
	bindProperty<int>(console, "AnimationManager.ignoreMotherOrientation", kAccess, [] { return 0; },
		[](const int&) {});

	// AnimationBundle.*
	const auto bundleFlag = [&console, bundle](const char* name, bool Bundle::*field) {
		bindProperty<bool>(console, std::string("AnimationBundle.") + name, kAccess,
			[bundle, field] { return bundle() ? bundle()->*field : false; },
			[bundle, field](const bool& v) {
				if (Bundle* b = bundle()) {
					b->*field = v;
				}
			});
	};
	bundleFlag("playBackward", &Bundle::playBackward);
	bundleFlag("jumpToLastAnimationAtStop", &Bundle::jumpToLastAnimationAtStop);
	bundleFlag("playForever", &Bundle::playForever);
	bundleFlag("isLooping", &Bundle::isLooping);
	bundleFlag("abruptPlayback", &Bundle::abruptPlayback);
	bundleFlag("useSpeedAsTime", &Bundle::useSpeedAsTime);
	bundleFlag("useNewSystem", &Bundle::useNewSystem);
	bundleFlag("blockInput", &Bundle::blockInput);
	const auto bundleNumber = [&console, bundle](const char* name, float Bundle::*field) {
		bindProperty<float>(console, std::string("AnimationBundle.") + name, kAccess,
			[bundle, field] { return bundle() ? bundle()->*field : 0.0f; },
			[bundle, field](const float& v) {
				if (Bundle* b = bundle()) {
					b->*field = v;
				}
			});
	};
	bundleNumber("length", &Bundle::length);
	bundleNumber("fadeInTime", &Bundle::fadeInTime);
	bundleNumber("fadeOutTime", &Bundle::fadeOutTime);
	bundleNumber("stickAround", &Bundle::stickAround);
	for (const char* name : {"forceStopPoints", "speedKeyPoints", "fadeOutSpeedKeyPoints", "fadeInSpeedKeyPoints"}) {
		// The new system's key points: not used yet.
		bindProperty<std::string>(console, std::string("AnimationBundle.") + name, kAccess,
			[] { return std::string(); }, [](const std::string&) {});
	}
	bindMethod(console, "AnimationBundle.addAnimation", kAccess, [system, bundle](std::string path) {
		AnimationSystemTemplate* t = system();
		Bundle* b = bundle();
		if (t && b) {
			b->clips.push_back({t->addClip(path), 0});
		}
	});
	bindMethod(console, "AnimationBundle.setAnimationStartTime", kAccess,
		[system, bundle](std::string path, float start) {
			AnimationSystemTemplate* t = system();
			Bundle* b = bundle();
			const int index = t ? t->findClip(path) : -1;
			if (b && index >= 0) {
				for (Bundle::Entry& entry : b->clips) {
					if (entry.clip == static_cast<size_t>(index)) {
						entry.startTime = start;
					}
				}
			}
		});
	bindMethod(console, "AnimationBundle.setAnimationLength", kAccess, [system](std::string path, float length) {
		AnimationSystemTemplate* t = system();
		const int index = t ? t->findClip(path) : -1;
		if (index >= 0) {
			t->clips()[static_cast<size_t>(index)].length = length;
		}
	});
	for (const char* name : {"addEvent", "removeEvent", "removeAnimation"}) {
		bindMethod(console, std::string("AnimationBundle.") + name, kAccess, [](std::string) {}); // events: not yet
	}
	bindMethod(console, "AnimationBundle.setEventStartTime", kAccess, [](std::string, float) {});

	// AnimationTrigger.*
	bindMethod(console, "AnimationTrigger.addChild", kAccess, [system, trigger](std::string name) {
		AnimationSystemTemplate* t = system();
		Trigger* parent = trigger();
		const int child = t ? t->findTrigger(name) : -1;
		if (parent && child >= 0) {
			parent->children.push_back(static_cast<size_t>(child));
			Trigger& added = t->triggers()[static_cast<size_t>(child)];
			added.isChild = true;
			// 0x008c5160: idle animations below an IdleTrigger.
			if (parent->type == TriggerType::Idle || parent->underIdle) {
				added.underIdle = true;
			}
		}
	});
	bindMethod(console, "AnimationTrigger.addBundle", kAccess, [system, trigger](std::string name) {
		AnimationSystemTemplate* t = system();
		Trigger* owner = trigger();
		const int b = t ? t->findBundle(name) : -1;
		if (owner && b >= 0) {
			owner->bundles.push_back(static_cast<size_t>(b));
		}
	});
	bindProperty<std::string>(console, "AnimationTrigger.valueHolder", kAccess, [] { return std::string(); },
		[s, trigger](const std::string& name) {
			if (Trigger* t = trigger()) {
				t->valueHolder = s->findValueHolder(name);
			}
		});
	bindMethod(console, "AnimationTrigger.clearValueHolder", kAccess, [trigger]() {
		if (Trigger* t = trigger()) {
			t->valueHolder = -1;
		}
	});
	bindProperty<float>(console, "AnimationTrigger.fadeInTime", kAccess,
		[trigger] { return trigger() ? trigger()->fadeInTime : 0.0f; },
		[trigger](const float& v) {
			if (Trigger* t = trigger()) {
				t->fadeInTime = v;
			}
		});
	bindProperty<int>(console, "AnimationTrigger.overridePrio", kAccess,
		[trigger] { return trigger() ? trigger()->overridePrio : -1; },
		[trigger](const int& v) {
			if (Trigger* t = trigger()) {
				t->overridePrio = v;
			}
		});
	bindProperty<int>(console, "AnimationTrigger.message", kAccess,
		[trigger] { return trigger() ? static_cast<int>(trigger()->message) : 0; },
		[trigger](const int& v) {
			if (Trigger* t = trigger()) {
				t->message = static_cast<uint32_t>(v);
			}
		});
	bindProperty<bool>(console, "AnimationTrigger.useDirection", kAccess,
		[trigger] { return trigger() && trigger()->useDirection; },
		[trigger](const bool& v) {
			if (Trigger* t = trigger()) {
				t->useDirection = v;
			}
		});
	bindProperty<bool>(console, "AnimationTrigger.triggerOnAcceleration", kAccess,
		[trigger] { return trigger() && trigger()->triggerOnAcceleration; },
		[trigger](const bool& v) {
			if (Trigger* t = trigger()) {
				t->triggerOnAcceleration = v;
			}
		});
	bindProperty<ConsoleVec2>(console, "AnimationTrigger.idleTime", kAccess,
		[trigger] { return trigger() ? ConsoleVec2 {trigger()->idleMin, trigger()->idleMax} : ConsoleVec2 {}; },
		[trigger](const ConsoleVec2& v) {
			if (Trigger* t = trigger()) {
				t->idleMin = v.x;
				t->idleMax = v.y;
				t->idleNext = v.x;
			}
		});
	bindProperty<int>(console, "AnimationTrigger.count", kAccess,
		[trigger] { return trigger() ? static_cast<int>(trigger()->count) : 0; },
		[trigger](const int& v) {
			if (Trigger* t = trigger()) {
				t->count = static_cast<uint32_t>(v);
			}
		});
	bindProperty<int>(console, "AnimationTrigger.switchId", kAccess,
		[trigger] { return trigger() ? static_cast<int>(trigger()->switchId) : 0; },
		[trigger](const int& v) {
			if (Trigger* t = trigger()) {
				t->switchId = static_cast<uint32_t>(v);
			}
		});

	// AnimationValueHolder.*
	bindMethod(console, "AnimationValueHolder.values", kAccess, [s](float min, float normal, float max) {
		if (ValueHolder* h = s->activeValueHolder()) {
			h->min = min;
			h->normal = normal;
			h->max = max;
		}
	});
	bindProperty<ConsoleVec2>(console, "AnimationValueHolder.clamp", kAccess,
		[s] { return s->activeValueHolder() ? ConsoleVec2 {s->activeValueHolder()->clampMin, s->activeValueHolder()->clampMax} : ConsoleVec2 {}; },
		[s](const ConsoleVec2& v) {
			if (ValueHolder* h = s->activeValueHolder()) {
				h->clampMin = v.x;
				h->clampMax = v.y;
				h->hasClamp = true;
			}
		});
	bindProperty<int>(console, "AnimationValueHolder.passOnMessage", kAccess,
		[s] { return s->activeValueHolder() ? static_cast<int>(s->activeValueHolder()->passOnMessage) : 0; },
		[s](const int& v) {
			if (ValueHolder* h = s->activeValueHolder()) {
				h->passOnMessage = static_cast<uint32_t>(v);
			}
		});
	bindProperty<int>(console, "AnimationValueHolder.stopOnMessage", kAccess,
		[s] { return s->activeValueHolder() ? static_cast<int>(s->activeValueHolder()->stopOnMessage) : 0; },
		[s](const int& v) {
			if (ValueHolder* h = s->activeValueHolder()) {
				h->stopOnMessage = static_cast<uint32_t>(v);
			}
		});
}

} // namespace dice::hfe::anim
