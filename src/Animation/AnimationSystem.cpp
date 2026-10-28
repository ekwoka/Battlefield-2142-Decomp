#include "AnimationSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::anim {

namespace {

std::string lower(std::string s)
{
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

bool same(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

constexpr float kPi = 3.14159265f;
constexpr int kExclusivePriority = 9000; // queuing above this stops all others (0x008bb770)

// Tweakables (registered at 0x00900a60-0x00900b80) at their defaults.
constexpr float kSurfaceInterpolate = 0.2f;  // anim_surfaceInterpolate
constexpr float kOffsetBackwardTime = 0.5f;  // anim_offsetBackwardTime
constexpr float kStepEdge = 0.25f;           // anim_test
constexpr float kStartLeftOffset = 0.2f;     // anim_startLeftOffset
constexpr float kStartRightOffset = 0.7f;    // anim_startRightOffset
constexpr float kSlopeBaseWeight = 1.0f;     // anim_test2
constexpr bool kSlopeForward = false;        // anim_test3 (an int, 0)

} // namespace

bool ValueHolder::inRange(float value) const
{
	if (min == max) {
		return true;
	}
	if (min >= 0) {
		return min <= value && value <= max;
	}
	return max <= value && value <= min;
}

Bundle::KeyPoints parseKeyPoints(const std::string& text)
{
	Bundle::KeyPoints points;
	points.text = text;
	size_t start = 0;
	while (start <= text.size()) {
		size_t end = text.find(';', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		if (end > start) {
			points.values.push_back(static_cast<float>(std::atof(text.substr(start, end - start).c_str())));
		}
		start = end + 1;
	}
	return points;
}

float keyPointValue(const std::vector<float>& points, float fraction)
{
	if (points.empty()) {
		return 1;
	}
	if (points.size() == 1) {
		return points[0];
	}
	// The original reads one past the end at a fraction of 1 (multiplied by
	// nothing) and beyond it outside 0..1; here the last segment is
	// extended instead.
	const float at = static_cast<float>(points.size() - 1) * fraction;
	const float whole = std::clamp(std::floor(at), 0.0f, static_cast<float>(points.size() - 2));
	const size_t i = static_cast<size_t>(whole);
	return (at - whole) * (points[i + 1] - points[i]) + points[i];
}

void Bundle::addClip(size_t clip)
{
	clips.insert(clips.begin(), Entry {clip, 0});
}

void Bundle::setClipStart(size_t clip, float startTime)
{
	auto it = std::find_if(clips.begin(), clips.end(), [clip](const Entry& e) { return e.clip == clip; });
	if (it == clips.end() || it->startTime == startTime) {
		return;
	}
	clips.erase(it);
	auto at = std::find_if(clips.begin(), clips.end(), [startTime](const Entry& e) { return startTime <= e.startTime; });
	clips.insert(at, Entry {clip, startTime});
}

Bundle::Event Bundle::parseEvent(const std::string& name)
{
	Event event;
	event.name = lower(name);
	const auto read = [&](const char* prefix) {
		const size_t n = std::char_traits<char>::length(prefix);
		// The original finds the prefix anywhere but reads the number with
		// sscanf from the start, which fails (leaving 0) unless it leads.
		return event.name.compare(0, n, prefix) == 0 ? std::atoi(event.name.c_str() + n) : 0;
	};
	if (event.name.find("effect:") != std::string::npos) {
		event.type = EffectEvent;
		event.id = read("effect:");
	} else if (event.name.find("sound:") != std::string::npos) {
		event.type = SoundEvent;
		event.id = read("sound:");
	}
	return event;
}

void Bundle::addEvent(const std::string& name)
{
	events.insert(events.begin(), parseEvent(name));
}

void Bundle::removeEvent(const std::string& name)
{
	const std::string key = lower(name);
	auto it = std::find_if(events.begin(), events.end(), [&key](const Event& e) { return e.name == key; });
	if (it != events.end()) {
		events.erase(it);
	}
}

void Bundle::setEventStart(const std::string& name, float startTime)
{
	const std::string key = lower(name);
	auto it = std::find_if(events.begin(), events.end(), [&key](const Event& e) { return e.name == key; });
	if (it == events.end() || it->startTime == startTime) {
		return;
	}
	Event event = *it;
	event.startTime = startTime;
	events.erase(it);
	auto at = std::find_if(events.begin(), events.end(), [startTime](const Event& e) { return startTime <= e.startTime; });
	events.insert(at, event);
}

void CameraSpring::overrideBy(const CameraSpring& other)
{
	for (auto [mine, theirs] : {std::pair {&amplitude, &other.amplitude}, {&zoomDamping, &other.zoomDamping},
			 {&stiffness, &other.stiffness}, {&move, &other.move}}) {
		if (!theirs->zero()) {
			*mine = *theirs;
		}
	}
	if (std::fabs(other.look[0]) + std::fabs(other.look[1]) >= 1.1920929e-07f) {
		look[0] = other.look[0];
		look[1] = other.look[1];
	}
	for (auto [mine, theirs] : {std::pair {&hit, &other.hit}, {&fire, &other.fire}}) {
		if (!theirs->zero()) {
			*mine = *theirs;
		}
	}
}

CameraSpring::V CameraSpringState::update(const CameraSpring& spring, const Input& input, float seconds)
{
	static constexpr float kSpringClamp = 0.001f; // anim-soldier-spring-clamp
	constexpr float kSpringZoomFactor = 6.0f; // anim-soldier-spring-zoom-factor
	// 0x005edda0
	const auto relax = [seconds](float& v, float stiffness) {
		const float step = std::max(v * v * stiffness * seconds, kSpringClamp);
		if (v > 0) {
			v -= step;
			if (v < 0.0001f) {
				v = 0;
			}
		} else if (v < 0) {
			v += step;
			if (v >= -0.0001f) {
				v = 0;
			}
		}
	};
	relax(value[0], spring.stiffness.x);
	relax(value[1], spring.stiffness.y);
	relax(value[2], spring.stiffness.z);
	value[0] -= spring.look[0] * input.yawDelta * seconds;
	value[1] += spring.look[1] * input.pitchDelta * seconds;
	// The sideways push also dips the rig (-0.3 of it); the forward speed
	// drives y by move.z.
	const float side = input.right * spring.move.x * seconds;
	value[0] += side;
	value[1] += std::fabs(side) * -0.3f - input.forward * spring.move.z * seconds;
	value[1] += input.up * spring.move.y * seconds;
	const float kick = input.recoil * seconds;
	value[0] += spring.fire.x * kick;
	value[1] += spring.fire.y * kick;
	value[2] += spring.fire.z * kick;
	for (float& v : value) {
		v = std::clamp(v, -1.0f, 1.0f);
	}
	// Zoomed, each amplitude goes towards amplitude / (zoomDamping x 6).
	CameraSpring::V amplitude = spring.amplitude;
	if (input.zoom > 0) {
		const float f = input.zoom;
		const auto damp = [f](float a, float d) {
			const float inverse = d * kSpringZoomFactor > 0 ? 1.0f / (d * kSpringZoomFactor) : 0.0f;
			return (1 - f) * a + inverse * a * f;
		};
		amplitude = {damp(amplitude.x, spring.zoomDamping.x), damp(amplitude.y, spring.zoomDamping.y),
			damp(amplitude.z, spring.zoomDamping.z)};
	}
	const auto tiny = [](float a) { return std::fabs(a) < 0.001f ? 0.0f : a; };
	return {tiny(amplitude.x) * value[0], tiny(amplitude.y) * value[1], tiny(amplitude.z) * value[2]};
}

float SoldierJumpAnimation::update(bool onGround, const float velocity[3],
	const std::function<bool(const float way[3])>& ground, Sound& sound)
{
	sound = Sound::None;
	if (onGround) {
		jumping = false;
		landing = false;
		return 0;
	}
	const float vy = velocity[1];
	if (landing) {
		return vy;
	}
	if (vy >= kStartLandAtSpeed) {
		if (jumping) {
			return 1;
		}
		if (vy >= kStartJumpAtSpeed) {
			jumping = true;
			landing = false;
			sound = Sound::Jump;
			return 1;
		}
	} else if (kStartLandAtHeight > 0) {
		float way[3] = {velocity[0] * kStartLandAtHeight, vy * kStartLandAtHeight, velocity[2] * kStartLandAtHeight};
		const float squared = way[0] * way[0] + way[1] * way[1] + way[2] * way[2];
		const float limit = kStartLandAtHeight * kStartLandAtHeight;
		if (squared != limit) {
			if (squared < limit) {
				way[1] = -1; // slow: straight down (the x and z kept, then normalised)
			}
			const float length = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
			for (float& w : way) {
				w *= kStartLandAtHeight / length;
			}
		}
		if (ground && ground(way)) {
			landing = true;
			sound = Sound::Land;
		}
	}
	if (!jumping) {
		return !landing && vy <= 0 ? vy : 0.0f;
	}
	return landing ? 0.0f : 1.0f;
}

bool parseTriggerType(const std::string& name, TriggerType& type)
{
	static const std::pair<const char*, TriggerType> names[] = {
		{"Trigger", TriggerType::Trigger},
		{"PoseTrigger", TriggerType::Pose},
		{"IdleTrigger", TriggerType::Idle},
		{"CountTrigger", TriggerType::Count},
		{"RandomTrigger", TriggerType::Random},
		{"MessageTrigger", TriggerType::Message},
		{"SwitchMessageTrigger", TriggerType::SwitchMessage},
		{"MovementTrigger", TriggerType::Movement},
		{"ForwardTrigger", TriggerType::Forward},
		{"SideTrigger", TriggerType::Side},
		{"UpTrigger", TriggerType::Up},
		{"TurnTrigger", TriggerType::Turn},
		{"LookAroundTrigger", TriggerType::LookAround},
		{"SlopeTrigger", TriggerType::Slope},
	};
	for (const auto& [text, value] : names) {
		if (same(name, text)) {
			type = value;
			return true;
		}
	}
	return false;
}

size_t AnimationSystemTemplate::addClip(const std::string& path)
{
	const int existing = findClip(path);
	if (existing >= 0) {
		return static_cast<size_t>(existing);
	}
	m_clips.push_back({path});
	return m_clips.size() - 1;
}

size_t AnimationSystemTemplate::addBundle(const std::string& name)
{
	const int existing = findBundle(name);
	if (existing >= 0) {
		return static_cast<size_t>(existing);
	}
	Bundle bundle;
	bundle.name = name;
	m_bundles.push_back(bundle);
	return m_bundles.size() - 1;
}

void AnimationSystemTemplate::useCameraSpring(bool use)
{
	if (!use) {
		m_cameraSpring.reset();
	} else if (!m_cameraSpring) {
		m_cameraSpring = std::make_unique<CameraSpring>();
	}
}

size_t AnimationSystemTemplate::addTrigger(TriggerType type, const std::string& name)
{
	const int existing = findTrigger(name);
	if (existing >= 0) {
		m_triggers[static_cast<size_t>(existing)].type = type;
		return static_cast<size_t>(existing);
	}
	Trigger trigger;
	trigger.name = name;
	trigger.type = type;
	if (type == TriggerType::Idle) {
		trigger.idleMin = 5; // 0x008c4db0
		trigger.idleMax = 10;
		trigger.idleNext = 5;
	}
	m_triggers.push_back(trigger);
	return m_triggers.size() - 1;
}

int AnimationSystemTemplate::findClip(const std::string& path) const
{
	for (size_t i = 0; i < m_clips.size(); ++i) {
		if (same(m_clips[i].path, path)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int AnimationSystemTemplate::findBundle(const std::string& name) const
{
	for (size_t i = 0; i < m_bundles.size(); ++i) {
		if (same(m_bundles[i].name, name)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int AnimationSystemTemplate::findTrigger(const std::string& name) const
{
	for (size_t i = 0; i < m_triggers.size(); ++i) {
		if (same(m_triggers[i].name, name)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void AnimationSystemTemplate::finish(const std::function<float(const Clip&)>& clipLength)
{
	// 0x008bea30: a bundle with a looping clip plays its clips in sequence;
	// 0x008be400: the bundle lasts until its last clip ends or its last
	// event, whichever is later.
	for (Bundle& bundle : m_bundles) {
		bundle.hasLoopingClip = std::any_of(bundle.clips.begin(), bundle.clips.end(),
			[this](const Bundle::Entry& entry) { return m_clips[entry.clip].looping; });
		if (bundle.clips.empty()) {
			continue;
		}
		float length = 0;
		for (const Bundle::Entry& entry : bundle.clips) {
			length = std::max(length, entry.startTime + clipLength(m_clips[entry.clip]));
		}
		for (const Bundle::Event& event : bundle.events) {
			length = std::max(length, event.startTime);
		}
		bundle.length = length > 0 ? length : 1;
	}
	// 0x008c0f50: the triggers that are nobody's child, by name, each
	// numbering its tree in pre-order; later numbers win.
	std::vector<size_t> roots;
	for (size_t i = 0; i < m_triggers.size(); ++i) {
		if (!m_triggers[i].isChild) {
			roots.push_back(i);
		}
	}
	std::sort(roots.begin(), roots.end(),
		[this](size_t a, size_t b) { return lower(m_triggers[a].name) < lower(m_triggers[b].name); });
	int counter = 0;
	std::function<void(size_t, int)> number = [&](size_t index, int depth) {
		if (depth > 64) {
			return;
		}
		m_triggers[index].priority = ++counter;
		for (size_t child : m_triggers[index].children) {
			number(child, depth + 1);
		}
	};
	for (size_t root : roots) {
		number(root, 0);
	}
}

AnimationSystemTemplate* AnimationSystems::get(const std::string& path, const std::function<void()>& build)
{
	auto& slot = m_systems[lower(path)];
	if (!slot) {
		slot = std::make_unique<AnimationSystemTemplate>(path);
		AnimationSystemTemplate* previous = m_active;
		m_active = slot.get();
		activeClip = -1;
		activeBundle = -1;
		activeTrigger = -1;
		if (build) {
			build();
		}
		m_active = previous;
		slot->finish([this](const Clip& clip) { return clipLength(clip); });
	}
	return slot.get();
}

ValueHolder& AnimationSystems::createValueHolder(const std::string& name)
{
	const int existing = findValueHolder(name);
	if (existing >= 0) {
		m_activeHolder = &m_holders[static_cast<size_t>(existing)].second;
	} else {
		m_holders.emplace_back(name, ValueHolder {});
		m_activeHolder = &m_holders.back().second;
	}
	return *m_activeHolder;
}

const ValueHolder* AnimationSystems::valueHolder(int index) const
{
	return index >= 0 && static_cast<size_t>(index) < m_holders.size() ? &m_holders[static_cast<size_t>(index)].second
																		: nullptr;
}

int AnimationSystems::findValueHolder(const std::string& name) const
{
	for (size_t i = 0; i < m_holders.size(); ++i) {
		if (same(m_holders[i].first, name)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

const Animation* AnimationSystems::animation(const std::string& path)
{
	auto& slot = m_animations[lower(path)];
	if (!slot) {
		std::vector<uint8_t> data;
		auto parsed = m_reader && m_reader(path, data) ? parseAnimation(data) : std::nullopt;
		slot = std::make_unique<Animation>(parsed ? std::move(*parsed) : Animation {});
	}
	return slot->frameCount > 0 ? slot.get() : nullptr;
}

const Skeleton* AnimationSystems::skeleton(const std::string& path)
{
	auto& slot = m_skeletons[lower(path)];
	if (!slot) {
		std::vector<uint8_t> data;
		auto parsed = m_reader && m_reader(path, data) ? parseSkeleton(data) : std::nullopt;
		slot = std::make_unique<Skeleton>(parsed ? std::move(*parsed) : Skeleton {});
	}
	return slot->nodes.empty() ? nullptr : slot.get();
}

const RagDollTemplate* AnimationSystems::ragDoll(const std::string& path)
{
	const std::string key = lower(path);
	const auto known = m_ragDolls.find(key);
	if (known != m_ragDolls.end()) {
		return known->second.get();
	}
	auto loaded = std::make_unique<RagDollTemplate>();
	const bool ok = loadRagDoll(*loaded, path,
		[this](const std::string& file, std::vector<uint8_t>& data) { return m_reader && m_reader(file, data); },
		[this](const std::string& file) { return skeleton(file); });
	auto& slot = m_ragDolls[key];
	if (ok) {
		slot = std::move(loaded);
	}
	return slot.get();
}

float AnimationSystems::clipLength(const Clip& clip)
{
	if (clip.length > 0) {
		return clip.length;
	}
	const Animation* a = animation(clip.path);
	return a ? static_cast<float>(a->frameCount) / kFramesPerSecond : 1.0f;
}

AnimationState::AnimationState(AnimationSystems& systems, const AnimationSystemTemplate& system)
	: m_systems(systems), m_system(system)
{
}

uint32_t AnimationState::nextRandom()
{
	m_random = m_random * 1103515245u + 12345u;
	return (m_random >> 16) & 0x7fff;
}

int AnimationState::priorityOf(const Trigger& trigger) const
{
	return trigger.overridePrio != -1 ? trigger.overridePrio : trigger.priority;
}

// The value a trigger tests (slot 0x48 of each type).
float AnimationState::value(const Trigger& trigger) const
{
	// By the factory (0x008c1170) and the types' vtables: UpTrigger is
	// 0x009dcde0 (slot 0x48 0x008c4980), TurnTrigger 0x009dce50 (0x008c49e0).
	switch (trigger.type) {
	case TriggerType::Forward: // 0x008c48d0
		return trigger.triggerOnAcceleration ? m_acceleration[2] : m_speed * m_direction[2];
	case TriggerType::Side: { // 0x008c4920
		const float v = trigger.triggerOnAcceleration ? m_acceleration[0] : m_speed * m_direction[0];
		return trigger.useDirection ? std::fabs(v) : v;
	}
	case TriggerType::Up: { // 0x008c4980
		const float v = trigger.triggerOnAcceleration ? m_acceleration[1] : m_direction[1];
		return trigger.useDirection ? std::fabs(v) : v;
	}
	case TriggerType::Turn: // 0x008c49e0
		return m_input.turn;
	default:
		return m_speed;
	}
}

// Slot 0x48: true unless the evaluation should stop (no trigger type here
// stops it, but the result passes up as in the original).
bool AnimationState::evaluate(size_t index)
{
	const Trigger& t = m_system.triggers()[index];
	// 0x008c3fb0, which every type but Pose, SwitchMessage and Count ends
	// in: a trigger with a fadeInTime fades the whole system in.
	const auto base = [&]() {
		if (t.fadeInTime > 0) {
			setFadingTrigger(index);
		}
		for (size_t child : t.children) {
			if (!evaluate(child)) {
				return false;
			}
		}
		queueBundles(index);
		return true;
	};
	const ValueHolder* holder = m_systems.valueHolder(t.valueHolder);
	const uint32_t messages = m_input.messages | m_input.pulses;
	switch (t.type) {
	case TriggerType::Pose: { // 0x008c4010: the child for the pose
		if (!t.children.empty()) {
			const size_t which = std::min(static_cast<size_t>(std::max(m_input.pose, 0)), t.children.size() - 1);
			if (!evaluate(t.children[which])) {
				return false;
			}
		}
		queueBundles(index);
		return true;
	}
	case TriggerType::Message: // 0x008c43c0
		if (t.message != 0 && (messages & t.message) == 0) {
			return true;
		}
		return base();
	case TriggerType::SwitchMessage: { // 0x008c43f0
		if (!t.children.empty()) {
			const size_t which = (messages & t.message) == 0 && t.children.size() > 1 ? 1 : 0;
			if (!evaluate(t.children[which])) {
				return false;
			}
		}
		if (!t.bundles.empty() && (t.message & messages) != (t.message & m_previousMessages)) {
			if (t.bundles.size() != 2) {
				queue(t.bundles[0], index, 10000, 1, (t.message & messages) != 0 ? 1.0f : -1.0f);
			} else if ((t.message & messages) == 0) {
				queue(t.bundles[0], index, 8000, 1, 1);
			}
		}
		return true;
	}
	case TriggerType::Movement: // 0x008c4670
		if (holder && ((holder->passOnMessage != 0 && (messages & holder->passOnMessage) == 0)
						  || (holder->stopOnMessage != 0 && (messages & holder->stopOnMessage) != 0))) {
			return true;
		}
		if (holder && !holder->inRange(m_speed)) {
			return true;
		}
		return base();
	case TriggerType::Forward:
	case TriggerType::Side:
	case TriggerType::Turn:
	case TriggerType::Up:
		if (holder && !holder->inRange(value(t))) {
			return true;
		}
		return base();
	case TriggerType::Idle: { // 0x008c41c0: after idleNext seconds with one bundle playing
		auto next = m_idleNext.find(index);
		if (next == m_idleNext.end()) {
			next = m_idleNext.emplace(index, t.idleNext).first;
		}
		if (m_idleTime != 0 && t.idleMin <= t.idleMax && m_idleTime >= next->second && m_idleTime <= t.idleMax) {
			next->second = (t.idleMax - t.idleMin) * static_cast<float>(nextRandom() % 100) * 0.01f + t.idleMin;
			return base();
		}
		return true;
	}
	case TriggerType::Count: { // 0x008c42b0
		// The first child until the bundle on top has wrapped or ended
		// once, then the second for good: the system remembers it under
		// switch 1 (slots 0x6c, 0x70 and 0x68), shared by every
		// CountTrigger of the system. The count property isn't read.
		if (t.children.size() != 2) {
			return true;
		}
		bool second = m_switches[1];
		if (!second) {
			second = !m_instances.empty() && m_instances.back().loops != 0;
			if (second) {
				m_switches[1] = true;
			}
		}
		return evaluate(t.children[second ? 1 : 0]);
	}
	default:
		return base();
	}
}

// Slot 0x4c: queue the trigger's bundles.
void AnimationState::queueBundles(size_t index)
{
	const Trigger& t = m_system.triggers()[index];
	if (t.bundles.empty()) {
		return;
	}
	const int priority = priorityOf(t);
	const ValueHolder* holder = m_systems.valueHolder(t.valueHolder);
	const float normal = holder && holder->normal != 0 ? holder->normal : 0;
	switch (t.type) {
	case TriggerType::Random: // 0x008c4090: one of them
		queue(t.bundles[nextRandom() % t.bundles.size()], index, priority, -1, 1);
		return;
	case TriggerType::Forward: // 0x008c3a70
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, normal != 0 ? m_speed / normal : 1);
		}
		return;
	case TriggerType::Side: { // 0x008c3b00
		float rate = normal != 0 ? m_speed / normal : 1;
		if (t.useDirection) {
			// The original multiplies by the direction's y (0x18), not its
			// x; no shipped SideTrigger sets useDirection.
			rate *= m_direction[1];
		}
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, rate);
		}
		return;
	}
	case TriggerType::Up: { // 0x008c3bb0: with useDirection, backwards going down (ladders)
		const float up = m_direction[1];
		float rate = normal > 0 ? std::fabs(up / normal) : 1;
		if (t.useDirection) {
			rate *= up > 0 ? 1.0f : (up < 0 ? -1.0f : 0.0f);
		}
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, rate);
		}
		return;
	}
	case TriggerType::Turn: // 0x008c3c60: turning on the spot starts the step at the left foot
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, normal != 0 ? std::fabs(m_input.turn) / normal : 1);
		}
		m_phase = kStartLeftOffset;
		return;
	case TriggerType::Movement: { // 0x008c46d0: the first bundle, by direction
		if (!holder && t.bundles.size() == 2) {
			// Starting to walk: the first bundle if the last movement
			// bundle was near the start or end of its step, else the
			// second; the step phase is set to where each starts.
			const bool first = m_phase < kStepEdge || 1 - kStepEdge < m_phase;
			m_phase = first ? kStartLeftOffset : kStartRightOffset;
			queue(t.bundles[first ? 0 : 1], index, priority, -1, 1);
			return;
		}
		const float rate = normal != 0 ? m_speed / normal : 1;
		Instance& instance = queue(t.bundles[0], index, priority, -1, rate);
		const Bundle& bundle = m_system.bundles()[t.bundles[0]];
		if (bundle.clips.size() == 4) {
			const float along = std::clamp(std::fabs(m_direction[2]), -1.0f, 1.0f);
			// Foot-phase matching: a bundle not yet moving by direction
			// starts where the last one was in its step.
			if (instance.strafe == -1) {
				instance.time = bundle.length * m_phase;
			}
			setDirection(instance, std::asin(along) * (2.0f / kPi), m_direction[0] >= 0.01f ? 2 : 3,
				m_direction[2] < -0.01f ? 1 : 0);
			m_phase = instance.time / bundle.length;
			if (m_direction[0] < -0.5f) {
				m_phase = kStartRightOffset;
			} else if (m_direction[0] > 0.5f) {
				m_phase = kStartLeftOffset;
			}
		}
		return;
	}
	case TriggerType::LookAround: { // 0x008c3d00
		// Bundles of two clips or more blend clip 0 with clip 1 by the
		// first look value; every bundle plays at a rate of the second
		// (with useSpeedAsTime, the time in the bundle). Both are mapped
		// from -1..1 to 0..1.
		const float x = std::clamp(m_input.lookAroundX, -1.0f, 1.0f);
		const float y = std::clamp(m_input.lookAroundY, -1.0f, 1.0f);
		for (size_t b : t.bundles) {
			Instance& instance = queue(b, index, priority, -1, y * 0.5f + 0.5f);
			setDirection(instance, x * 0.5f + 0.5f, 0, 1);
		}
		return;
	}
	case TriggerType::Slope: { // 0x008c4f00
		// The rate is clamped by the value holder's clamp (0..0 unless
		// set). Bundles of five clips draw the level clip (index 0) and,
		// over it, the one leaning to the side by |slope| (1 for a
		// positive slope, 2 for a negative one). The forward lean (3 and
		// 4) is behind anim_test3, off by default.
		float rate = 1;
		if (holder && holder->normal != 0) {
			rate = std::max(std::min(m_speed / holder->normal, holder->clampMax), holder->clampMin);
		}
		for (size_t b : t.bundles) {
			Instance& instance = queue(b, index, priority, -1, rate);
			if (m_system.bundles()[b].clips.size() != 5) {
				continue;
			}
			instance.layers.clear();
			instance.layers.emplace_back(0, kSlopeBaseWeight);
			if (m_slopeSide != 0) {
				instance.layers.emplace_back(m_slopeSide > 0 ? 1 : 2, std::fabs(m_slopeSide));
			}
			if (kSlopeForward && m_slopeForward != 0) {
				instance.layers.emplace_back(m_slopeForward > 0 ? 3 : 4, std::fabs(m_slopeForward));
			}
		}
		return;
	}
	default: // 0x008c39f0
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, 1);
		}
		return;
	}
}

// 0x008bb770 with 0x008c2950 and 0x008c29d0.
AnimationState::Instance& AnimationState::queue(size_t bundleIndex, size_t trigger, int priority, float weightTarget, float rate)
{
	if (priority > kExclusivePriority) {
		m_instances.clear();
	}
	const Bundle& bundle = m_system.bundles()[bundleIndex];
	auto it = std::find_if(m_instances.begin(), m_instances.end(),
		[bundleIndex](const Instance& i) { return i.bundle == bundleIndex; });
	if (it == m_instances.end()) {
		auto at = std::find_if(m_instances.begin(), m_instances.end(),
			[priority](const Instance& i) { return priority < i.priority; });
		Instance fresh;
		fresh.bundle = bundleIndex;
		fresh.trigger = trigger;
		fresh.priority = priority;
		fresh.weight = bundle.fadeInTime > 0 ? 0.0f : 1.0f; // 0x008c2c50
		fresh.fadeInLeft = bundle.fadeInTime;
		it = m_instances.insert(at, fresh);
	}
	Instance& instance = *it;
	// 0x008c2950
	if (weightTarget >= 0 && bundle.isLooping) {
		instance.weightTarget = weightTarget;
	}
	instance.rate = bundle.jumpToLastAnimationAtStop ? 1.0f : rate;
	if (!bundle.isLooping) {
		instance.fade = 1;
		if (rate < 0 && !instance.active) {
			instance.time = bundle.length;
		}
	}
	instance.stopping = false; // queued again: no longer playing on to a stop
	// 0x008c29d0: a one-shot (without a looping clip) queued again while
	// it plays starts over with abruptPlayback (looping bundles and those
	// with a looping clip aren't active between updates).
	if (instance.active && bundle.abruptPlayback && !bundle.hasLoopingClip) {
		instance.time = 0;
	}
	instance.active = true;
	instance.priority = priority;
	return instance;
}

// 0x008c2b90: directional clips (bundles of more than one clip). Changing
// the strafe clip while it dominates (blend under 0.3), or the forward /
// backward one while that does (over 0.7), fades the bundle's pose in
// again over its fadeInTime (switchFade, see apply).
void AnimationState::setDirection(Instance& instance, float blend, int strafe, int forwardOrBack) const
{
	const Bundle& bundle = m_system.bundles()[instance.bundle];
	if (bundle.clips.size() <= 1) {
		return;
	}
	instance.blend = blend;
	if (instance.strafe != -1
		&& ((blend > 0.7f && instance.forwardOrBack != forwardOrBack) || (blend < 0.3f && instance.strafe != strafe))
		&& bundle.fadeInTime > 0) {
		instance.switchFade = 0;
	}
	instance.forwardOrBack = forwardOrBack;
	instance.strafe = strafe;
}

// 0x008b9720
void AnimationState::setFadingTrigger(size_t trigger)
{
	if (m_fadingTrigger != static_cast<int>(trigger)) {
		m_fadingTrigger = static_cast<int>(trigger);
		m_fadeLeft = m_system.triggers()[trigger].fadeInTime;
	}
}

// 0x008be470: a bundle's clip by index in its list (the last added first).
const Clip* AnimationState::bundleClip(const Bundle& bundle, int index) const
{
	if (index < 0 || static_cast<size_t>(index) >= bundle.clips.size()) {
		return nullptr;
	}
	return &m_system.clips()[bundle.clips[static_cast<size_t>(index)].clip];
}

// 0x008c3360: false when the instance has finished.
bool AnimationState::advance(Instance& instance, float seconds)
{
	const Bundle& bundle = m_system.bundles()[instance.bundle];
	if (bundle.useNewSystem) {
		return advanceNew(instance, seconds);
	}
	if (!instance.active && bundle.stickAround > 0 && instance.stickLeft > 0) {
		instance.stickLeft -= seconds;
		return instance.stickLeft >= 0;
	}
	const auto finished = [&]() {
		instance.active = false;
		instance.weight = 0;
		instance.fade = 0;
		if (bundle.stickAround <= 0) {
			return false;
		}
		instance.stickLeft = bundle.stickAround;
		return true;
	};
	fireEvents(instance);
	if (!bundle.useSpeedAsTime) {
		instance.time += seconds * instance.rate;
		if (instance.time > bundle.length) {
			if (bundle.isLooping) {
				++instance.loops;
				instance.time -= bundle.length;
				instance.nextEvent = 0; // the events fire again each loop
			} else {
				instance.loops = 1;
				if (bundle.playForever) {
					instance.time = bundle.length;
				} else if (!finished()) {
					return false;
				}
			}
		} else if (instance.time < 0) {
			if (bundle.isLooping) {
				++instance.loops;
				instance.time += bundle.length;
			} else {
				instance.loops = 1;
				instance.time = 0;
				if (!bundle.playForever && !finished()) {
					return false;
				}
			}
		}
		if (bundle.hasLoopingClip) {
			if (!instance.active) {
				// No longer queued: abruptPlayback skips to the end of the
				// last looping clip (the jump's loop, on to the landing),
				// remembering how far for drawing it (0x40).
				if (bundle.abruptPlayback) {
					for (auto entry = bundle.clips.rbegin(); entry != bundle.clips.rend(); ++entry) {
						const Clip& clip = m_system.clips()[entry->clip];
						if (!clip.looping) {
							continue;
						}
						const float end = entry->startTime + m_systems.clipLength(clip);
						if (instance.time < end) {
							instance.jumpOffset = end - instance.time;
							instance.time = end;
						}
						break;
					}
				}
			} else {
				// Queued: the looping clips repeat.
				for (const Bundle::Entry& entry : bundle.clips) {
					const Clip& clip = m_system.clips()[entry.clip];
					const float end = entry.startTime + m_systems.clipLength(clip);
					if (clip.looping && end < instance.time) {
						instance.time = instance.time - end + entry.startTime;
					}
				}
			}
		}
	} else {
		instance.time = std::clamp(instance.rate, 0.0f, 1.0f) * bundle.length;
	}
	// Fading out: one-shots near their end, the others once not queued.
	bool fadingOut;
	if (!bundle.useSpeedAsTime && !bundle.isLooping) {
		fadingOut = instance.rate < 0 ? instance.time < bundle.fadeOutTime
									  : bundle.length - bundle.fadeOutTime < instance.time;
	} else {
		fadingOut = !instance.active;
	}
	if (bundle.stickAround <= 0 && fadingOut) {
		if (instance.weight > 0 && bundle.fadeOutTime > 0) {
			const float fade = instance.fade - seconds / bundle.fadeOutTime;
			if (fade > 0) {
				instance.fade = std::clamp(fade, 0.0f, 1.0f);
				instance.weight = instance.fade * instance.fade;
				return true;
			}
		}
		instance.active = false;
		instance.weight = 0;
		instance.fade = 0;
		return false;
	}
	// The fade after the directional clips changed (0x3c), linear over the
	// bundle's fadeInTime.
	if (bundle.fadeInTime > 0 && instance.switchFade < 1) {
		instance.switchFade = std::clamp(instance.switchFade + seconds / bundle.fadeInTime, 0.0f, 1.0f);
	}
	if (bundle.fadeInTime <= 0 || instance.weight >= 1) {
		instance.fade = 1;
		instance.weight = 1;
		return true;
	}
	instance.fade = std::clamp(instance.fade + seconds / bundle.fadeInTime, 0.0f, 1.0f);
	instance.weight = instance.fade * instance.fade;
	return true;
}

// 0x008c30a0: the new system. Fades are linear and timed in the bundle's
// own time (seconds times the rate); the weight lags the fade timers by
// one update, as in the original. A bundle no longer queued plays on to
// the next of its force stop points (a fraction of its length; past the
// last one, through the end into the next loop) and only then ends; one
// with none ends at once. Ending (0x008c2a40) holds the last frame with
// playForever, else starts the fade out, else the stick-around time.
bool AnimationState::advanceNew(Instance& instance, float seconds)
{
	const Bundle& bundle = m_system.bundles()[instance.bundle];
	instance.still = seconds <= 0;
	const auto end = [&]() {
		++instance.loops;
		if (bundle.playForever) {
			instance.time = bundle.length;
			return true;
		}
		instance.active = false;
		if (bundle.fadeOutTime > 0) {
			instance.fadeOutLeft = bundle.fadeOutTime;
			return true;
		}
		if (bundle.stickAround > 0) {
			instance.stickLeft = bundle.stickAround;
			return true;
		}
		return false;
	};
	if (instance.active || instance.fadeOutLeft > 0 || instance.fadeInLeft > 0 || instance.stickLeft > 0) {
		if (instance.stopping && instance.active) {
			// Wanted again on the way to a stop (queuing already clears
			// the stop, so this is not reached through queue): fade back
			// in from where the fade out is.
			if (instance.fadeOutLeft > 0) {
				instance.fadeInLeft = (1 - instance.fadeOutLeft / bundle.fadeOutTime) * bundle.fadeInTime;
			}
			instance.stopping = false;
		}
	} else if (!instance.stopping) {
		const std::vector<float>& points = bundle.forceStopPoints.values;
		if (points.empty()) {
			return end();
		}
		const float fraction = instance.time / bundle.length;
		instance.stopAt = fraction;
		for (float point : points) {
			if (fraction < point) {
				instance.stopAt = point;
				instance.stopping = true;
				break;
			}
		}
	}
	fireEvents(instance);
	const float step = seconds * instance.rate;
	instance.time += step;
	if (instance.stopping && instance.stopAt < instance.time / bundle.length) {
		instance.stopping = false;
		return end();
	}
	if (bundle.length < instance.time || instance.time < 0) {
		if (instance.stopping || !bundle.isLooping) {
			instance.loops = 1;
			return end();
		}
		++instance.loops;
		const float sign = instance.rate > 0 ? 1.0f : (instance.rate < 0 ? -1.0f : 0.0f);
		instance.time -= sign * bundle.length;
		instance.nextEvent = 0;
	}
	if (instance.fadeOutLeft > 0) {
		instance.weight = instance.fadeOutLeft / bundle.fadeOutTime;
		instance.fadeOutLeft -= step;
		return instance.fadeOutLeft > 0;
	}
	if (instance.stickLeft > 0) {
		instance.stickLeft -= step;
		return instance.stickLeft > 0;
	}
	if (instance.fadeInLeft > 0) {
		instance.weight = 1 - instance.fadeInLeft / bundle.fadeInTime;
		instance.fadeInLeft -= step;
		if (instance.fadeInLeft <= 0) {
			instance.fadeInLeft = 0;
		}
		return true;
	}
	instance.weight = 1;
	return true;
}

// 0x008b9ae0: the object's movement. Given a time, the direction's x and z
// are eased towards the new ones by anim_smoothSet (unless one swaps sign
// past 0.9, or the new direction or speed is nil) and the accelerations
// are taken: the old speed x direction less the new, per second (the
// original's sign).
void AnimationState::setMovement(const AnimationInput& input)
{
	constexpr float kSmoothSet = 10.0f; // anim_smoothSet
	const float wanted[3] = {input.side, input.upSpeed, input.forward};
	const float seconds = input.movementSeconds;
	const auto set = [&]() {
		m_direction[0] = wanted[0];
		m_direction[1] = wanted[1];
		m_direction[2] = wanted[2];
	};
	if (seconds <= 0) {
		m_speed = input.speed;
		set();
		return;
	}
	const float oldSpeed = m_speed;
	const float old[3] = {m_direction[0], m_direction[1], m_direction[2]};
	const auto sign = [](float v) { return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f); };
	if ((wanted[0] == 0 && wanted[2] == 0) || input.speed == 0) {
		set();
	} else if ((std::fabs(wanted[2]) > 0.9f && sign(wanted[2]) != sign(old[2]))
		|| (std::fabs(wanted[0]) > 0.9f && sign(wanted[0]) != sign(old[0]))) {
		set();
	} else {
		const float k = std::clamp(seconds * kSmoothSet, 0.0f, 1.0f);
		float x = k * wanted[0] + (1 - k) * old[0];
		float z = k * wanted[2] + (1 - k) * old[2];
		const float squared = x * x + z * z; // 0x004ca270
		if (std::fabs(squared - 1) >= 1.1920929e-07f) {
			if (std::fabs(squared) < 1e-12f) {
				x = z = 0;
			} else {
				x /= std::sqrt(squared);
				z /= std::sqrt(squared);
			}
		}
		m_direction[0] = x;
		m_direction[1] = wanted[1];
		m_direction[2] = z;
	}
	m_speed = input.speed;
	for (int i = 0; i < 3; ++i) {
		m_acceleration[i] = (oldSpeed * old[i] - m_speed * m_direction[i]) / seconds;
	}
}

// 0x008c3030 (and inline in 0x008c3360): every event at or before the
// bundle's time fires once; looping starts them over.
void AnimationState::fireEvents(Instance& instance)
{
	const Bundle& bundle = m_system.bundles()[instance.bundle];
	while (instance.nextEvent < bundle.events.size() && bundle.events[instance.nextEvent].startTime <= instance.time) {
		const Bundle::Event& event = bundle.events[instance.nextEvent++];
		if (event.type == Bundle::SoundEvent || event.type == Bundle::EffectEvent) {
			m_events.push_back({event.type, event.id, instance.bundle});
		}
	}
}

void AnimationState::update(const AnimationInput& input, float seconds)
{
	m_input = input;
	setMovement(input);
	// 0x008b9970: the slope is smoothed each time it is set.
	m_slopeForward += (input.slopeForward - m_slopeForward) * kSurfaceInterpolate;
	m_slopeSide += (input.slopeSide - m_slopeSide) * kSurfaceInterpolate;
	const uint32_t messages = input.messages | input.pulses;
	if (!m_started) {
		m_started = true;
		const int startup = m_system.findTrigger("startup");
		if (startup >= 0) {
			evaluate(static_cast<size_t>(startup));
			m_startupPlaying = true;
		}
	}
	for (const char* name : {"root", "postRoot"}) {
		const int trigger = m_system.findTrigger(name);
		if (trigger >= 0) {
			evaluate(static_cast<size_t>(trigger));
		}
	}
	const bool messagesChanged = messages != m_previousMessages;
	m_previousMessages = messages;
	// The system's weight while a trigger with a fadeInTime fades it in
	// (not while the startup trigger's bundles play).
	m_weight = 1;
	if (m_startupPlaying) {
		m_fadeLeft = 0;
	} else if (m_fadeLeft > 0 && m_fadingTrigger >= 0) {
		m_fadeLeft = std::max(m_fadeLeft - seconds, 0.0f);
		m_weight = 1 - m_fadeLeft / m_system.triggers()[static_cast<size_t>(m_fadingTrigger)].fadeInTime;
	}
	// A bundle with force stop points that is neither fading in nor out
	// holds every instance of another priority that isn't fading out
	// (the last such bundle by priority; priority 0 counts as none).
	int stopPriority = 0;
	for (const Instance& instance : m_instances) {
		if (!m_system.bundles()[instance.bundle].forceStopPoints.values.empty() && instance.fadeOutLeft <= 0
			&& instance.fadeInLeft <= 0) {
			stopPriority = instance.priority;
		}
	}
	m_idleTime = m_instances.size() == 1 ? m_idleTime + seconds : 0.0f;
	for (size_t i = 0; i < m_instances.size();) {
		Instance& instance = m_instances[i];
		const Bundle& bundle = m_system.bundles()[instance.bundle];
		const bool idle = m_system.triggers()[instance.trigger].underIdle;
		const bool last = i + 1 == m_instances.size();
		// 0x008c2a10: the startup's bundles are done once one isn't queued
		// or a one-shot reaches its fade out.
		if (m_startupPlaying
			&& (!instance.active
				|| (!bundle.useSpeedAsTime && !bundle.isLooping && bundle.length - bundle.fadeOutTime < instance.time))) {
			m_startupPlaying = false;
		}
		const bool runs = stopPriority == 0 || instance.priority == stopPriority || instance.fadeOutLeft > 0;
		const bool alive = advance(instance, runs ? seconds : 0.0f);
		if (!alive || (idle && (messagesChanged || !last))) {
			if (idle) {
				m_fadingTrigger = -1;
			}
			m_startupPlaying = false;
			m_instances.erase(m_instances.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}
		if (bundle.isLooping || bundle.hasLoopingClip) {
			instance.active = false; // must be queued again (0x008c29d0 with 0)
		}
		++i;
	}
}

bool AnimationState::play(const std::string& name)
{
	const int trigger = m_system.findTrigger(name);
	if (trigger < 0) {
		return false;
	}
	evaluate(static_cast<size_t>(trigger));
	return true;
}

std::vector<std::string> AnimationState::playing() const
{
	std::vector<std::string> names;
	for (const Instance& instance : m_instances) {
		names.push_back(m_system.bundles()[instance.bundle].name);
	}
	return names;
}

bool AnimationState::blocksInput() const
{
	for (const Instance& instance : m_instances) {
		if (m_system.bundles()[instance.bundle].blockInput) {
			return true;
		}
	}
	return false;
}

// 0x008ba420 with 0x008c2d70 per instance: the fade-in key points over the
// fade in, the fade-out ones over the fade out, else the speed key points
// over the bundle's length; 1 for instances held still.
float AnimationState::speedFactor() const
{
	float factor = 1;
	for (const Instance& instance : m_instances) {
		const Bundle& bundle = m_system.bundles()[instance.bundle];
		float value;
		if (instance.still) {
			value = 1;
		} else if (instance.fadeInLeft > 0 && !bundle.fadeInSpeedKeyPoints.values.empty()) {
			value = keyPointValue(bundle.fadeInSpeedKeyPoints.values, 1 - instance.fadeInLeft / bundle.fadeInTime);
		} else if (instance.fadeOutLeft > 0 && !bundle.fadeOutSpeedKeyPoints.values.empty()) {
			value = keyPointValue(bundle.fadeOutSpeedKeyPoints.values, 1 - instance.fadeOutLeft / bundle.fadeOutTime);
		} else {
			value = keyPointValue(bundle.speedKeyPoints.values, instance.time / bundle.length);
		}
		factor *= value;
	}
	return factor;
}

// 0x008c2e50 and the per-bone blend of 0x008b8330.
void AnimationState::apply(const Skeleton& skeleton, LocalPose& pose, std::vector<bool>& touched) const
{
	touched.resize(skeleton.nodes.size(), false);
	// Each bone this system moves is finally blended from what it was
	// before by the system's weight times the factors of the entries that
	// moved it (directional clips: their switch fade; others 1).
	const LocalPose before = pose;
	std::vector<float> factors(skeleton.nodes.size(), -1.0f);
	const auto blendInto = [&](size_t node, const Quaternion& rotation, const Position& position, float weight,
							   float factor) {
		factors[node] = (factors[node] < 0 ? 1.0f : factors[node]) * factor;
		if (!touched[node] || weight >= 1) {
			pose.rotations[node] = rotation;
			pose.positions[node] = position;
			touched[node] = true;
			return;
		}
		pose.rotations[node] = slerp(pose.rotations[node], rotation, weight);
		for (size_t i = 0; i < 3; ++i) {
			pose.positions[node][i] += (position[i] - pose.positions[node][i]) * weight;
		}
	};
	const auto drawClip = [&](const Clip& clip, float local, float weight) {
		const Animation* animation = m_systems.animation(clip.path);
		if (!animation) {
			return;
		}
		const float fraction = local / m_systems.clipLength(clip);
		for (const BoneTrack& track : animation->bones) {
			const size_t node = track.node;
			if (node >= skeleton.nodes.size()) {
				continue;
			}
			Quaternion r;
			Position p;
			if (sampleNode(*animation, node, fraction, clip.looping, r, p)) {
				blendInto(node, r, p, weight, 1);
			}
		}
	};
	for (const Instance& instance : m_instances) {
		const float weight = instance.weight * instance.weightTarget;
		if (weight <= 0) {
			continue;
		}
		const Bundle& bundle = m_system.bundles()[instance.bundle];
		const float time = bundle.playBackward ? bundle.length - instance.time : instance.time;
		if (!instance.layers.empty()) {
			// SlopeTrigger's layers (0x008c2a90 each, from time 0): none
			// while held still.
			if (instance.still) {
				continue;
			}
			for (const auto& [index, layerWeight] : instance.layers) {
				const Clip* clip = bundleClip(bundle, index);
				if (clip && weight * layerWeight > 0) {
					drawClip(*clip, time, std::min(weight * layerWeight, 1.0f));
				}
			}
			continue;
		}
		if (instance.strafe >= 0) {
			// 0x008c2b20: the strafe clip blended with the forward or
			// backward one; the backward clip runs anim_offsetBackwardTime
			// of the strafe clip's length ahead (not with useSpeedAsTime).
			const Clip* strafe = bundleClip(bundle, instance.strafe);
			const Clip* along = bundleClip(bundle, instance.forwardOrBack);
			if (!strafe || !along || weight <= 0.001f) {
				continue;
			}
			const Animation* a = m_systems.animation(strafe->path);
			const Animation* b = m_systems.animation(along->path);
			if (!a || !b) {
				continue;
			}
			const float strafeLength = m_systems.clipLength(*strafe);
			const float offset =
				!bundle.useSpeedAsTime && instance.forwardOrBack == 1 ? strafeLength * kOffsetBackwardTime : 0.0f;
			const float fa = time / strafeLength;
			const float fb = (time + offset) / m_systems.clipLength(*along);
			for (size_t node = 0; node < skeleton.nodes.size(); ++node) {
				Quaternion ra, rb;
				Position pa, pb;
				const bool hasA = sampleNode(*a, node, fa, strafe->looping, ra, pa);
				const bool hasB = sampleNode(*b, node, fb, along->looping, rb, pb);
				if (!hasA && !hasB) {
					continue;
				}
				if (!hasA) {
					ra = rb;
					pa = pb;
				} else if (!hasB) {
					rb = ra;
					pb = pa;
				}
				Position p;
				for (size_t i = 0; i < 3; ++i) {
					p[i] = pa[i] + (pb[i] - pa[i]) * instance.blend;
				}
				blendInto(node, slerp(ra, rb, instance.blend), p, std::min(weight, 1.0f), instance.switchFade);
			}
			continue;
		}
		if (instance.still) {
			continue; // 0x008c2a90 draws nothing for a held new-system bundle
		}
		// 0x008c2e50: the clips by start time up to the bundle's time. A
		// clip is drawn while it runs (or for good with playForever); past
		// its end only a looping one is, shifted by how far abruptPlayback
		// skipped. Each is drawn until the next has faded in, fading in
		// over its own fadeInTime (0x008c2a90: played backwards from the
		// bundle's end).
		for (size_t i = 0; i < bundle.clips.size(); ++i) {
			const Bundle::Entry& entry = bundle.clips[i];
			float start = entry.startTime;
			if (instance.time < start) {
				break;
			}
			const Clip& clip = m_system.clips()[entry.clip];
			const float end = start + m_systems.clipLength(clip);
			if (!(instance.time < end || bundle.playForever)) {
				if (!(instance.time <= end || clip.looping)) {
					continue;
				}
				start += instance.jumpOffset;
			}
			if (i + 1 < bundle.clips.size()) {
				const Bundle::Entry& next = bundle.clips[i + 1];
				if (next.startTime + m_system.clips()[next.clip].fadeInTime < instance.time) {
					continue;
				}
			}
			const float local = instance.time - start;
			float clipWeight = weight;
			if (clip.fadeInTime > 0 && local < clip.fadeInTime) {
				clipWeight *= local / clip.fadeInTime;
			}
			drawClip(clip, bundle.playBackward ? bundle.length - local : local, clipWeight);
		}
	}
	for (size_t node = 0; node < factors.size(); ++node) {
		if (factors[node] < 0) {
			continue;
		}
		const float k = m_weight * factors[node];
		if (k >= 1) {
			continue;
		}
		pose.rotations[node] = slerp(before.rotations[node], pose.rotations[node], k);
		for (size_t i = 0; i < 3; ++i) {
			pose.positions[node][i] = before.positions[node][i] + (pose.positions[node][i] - before.positions[node][i]) * k;
		}
	}
}

} // namespace dice::hfe::anim
