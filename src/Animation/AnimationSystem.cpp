#include "AnimationSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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
	// 0x008be400: the bundle lasts until its last clip ends.
	for (Bundle& bundle : m_bundles) {
		if (bundle.clips.empty()) {
			continue;
		}
		float length = 0;
		for (const Bundle::Entry& entry : bundle.clips) {
			length = std::max(length, entry.startTime + clipLength(m_clips[entry.clip]));
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
	switch (trigger.type) {
	case TriggerType::Forward: // 0x008c48d0
		return m_input.speed * m_input.forward;
	case TriggerType::Side: { // 0x008c4920
		const float v = m_input.speed * m_input.side;
		return trigger.useDirection ? std::fabs(v) : v;
	}
	case TriggerType::Turn: // 0x008c4980
		return trigger.useDirection ? std::fabs(m_input.turn) : m_input.turn;
	case TriggerType::Up: // 0x008c49e0
		return m_input.upSpeed;
	default:
		return m_input.speed;
	}
}

// Slot 0x48: true unless the evaluation should stop (no trigger type here
// stops it, but the result passes up as in the original).
bool AnimationState::evaluate(size_t index)
{
	const Trigger& t = m_system.triggers()[index];
	const auto base = [&]() {
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
		if (holder && !holder->inRange(m_input.speed)) {
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
	case TriggerType::Count: // 0x008c42b0 (the count itself is not kept)
		return t.children.size() == 2 ? evaluate(t.children[0]) : true;
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
			queue(b, index, priority, -1, normal != 0 ? m_input.speed / normal : 1);
		}
		return;
	case TriggerType::Side: { // 0x008c3b00
		float rate = normal != 0 ? m_input.speed / normal : 1;
		if (t.useDirection) {
			rate *= m_input.side;
		}
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, rate);
		}
		return;
	}
	case TriggerType::Turn: { // 0x008c3bb0
		float rate = normal > 0 ? std::fabs(m_input.turn / normal) : 1;
		if (t.useDirection) {
			rate *= m_input.turn < 0 ? -1.0f : 1.0f;
		}
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, rate);
		}
		return;
	}
	case TriggerType::Up: // 0x008c3c60
		for (size_t b : t.bundles) {
			queue(b, index, priority, -1, normal != 0 ? std::fabs(m_input.upSpeed) / normal : 1);
		}
		return;
	case TriggerType::Movement: { // 0x008c46d0: the first bundle, by direction
		const float rate = normal != 0 ? m_input.speed / normal : 1;
		Instance& instance = queue(t.bundles[0], index, priority, -1, rate);
		const Bundle& bundle = m_system.bundles()[t.bundles[0]];
		if (bundle.clips.size() == 4) {
			const float along = std::clamp(std::fabs(m_input.forward), -1.0f, 1.0f);
			instance.blend = std::asin(along) * (2.0f / kPi);
			instance.strafe = m_input.side >= 0.01f ? 2 : 3;
			instance.forwardOrBack = m_input.forward < -0.01f ? 1 : 0;
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
		fresh.weight = bundle.fadeInTime > 0 ? 0.0f : 1.0f;
		it = m_instances.insert(at, fresh);
	}
	Instance& instance = *it;
	if (weightTarget >= 0 && bundle.isLooping) {
		instance.weightTarget = weightTarget;
	}
	instance.rate = rate;
	if (!bundle.isLooping) {
		instance.fade = 1;
		if (rate < 0 && !instance.active) {
			instance.time = bundle.length;
		}
	}
	instance.active = true;
	instance.priority = priority;
	return instance;
}

// 0x008c3360: false when the instance has finished.
bool AnimationState::advance(Instance& instance, float seconds)
{
	const Bundle& bundle = m_system.bundles()[instance.bundle];
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
	if (!bundle.useSpeedAsTime) {
		instance.time += seconds * instance.rate;
		if (instance.time > bundle.length) {
			if (bundle.isLooping) {
				instance.time -= bundle.length;
			} else if (bundle.playForever) {
				instance.time = bundle.length;
			} else if (!finished()) {
				return false;
			}
		} else if (instance.time < 0) {
			if (bundle.isLooping) {
				instance.time += bundle.length;
			} else {
				instance.time = 0;
				if (!bundle.playForever && !finished()) {
					return false;
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
	if (bundle.fadeInTime <= 0 || instance.weight >= 1) {
		instance.fade = 1;
		instance.weight = 1;
		return true;
	}
	instance.fade = std::clamp(instance.fade + seconds / bundle.fadeInTime, 0.0f, 1.0f);
	instance.weight = instance.fade * instance.fade;
	return true;
}

void AnimationState::update(const AnimationInput& input, float seconds)
{
	m_input = input;
	const uint32_t messages = input.messages | input.pulses;
	if (!m_started) {
		m_started = true;
		const int startup = m_system.findTrigger("startup");
		if (startup >= 0) {
			evaluate(static_cast<size_t>(startup));
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
	m_idleTime = m_instances.size() == 1 ? m_idleTime + seconds : 0.0f;
	for (size_t i = 0; i < m_instances.size();) {
		Instance& instance = m_instances[i];
		const Bundle& bundle = m_system.bundles()[instance.bundle];
		const bool idle = m_system.triggers()[instance.trigger].underIdle;
		const bool last = i + 1 == m_instances.size();
		const bool alive = advance(instance, seconds);
		if (!alive || (idle && (messagesChanged || !last))) {
			m_instances.erase(m_instances.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}
		if (bundle.isLooping) {
			instance.active = false; // must be queued again, or fades out
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

// 0x008c2e50 and the per-bone blend of 0x008b8530.
void AnimationState::apply(const Skeleton& skeleton, LocalPose& pose, std::vector<bool>& touched) const
{
	touched.resize(skeleton.nodes.size(), false);
	const auto blendInto = [&](size_t node, const Quaternion& rotation, const Position& position, float weight) {
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
	for (const Instance& instance : m_instances) {
		const float weight = instance.weight * instance.weightTarget;
		if (weight <= 0) {
			continue;
		}
		const Bundle& bundle = m_system.bundles()[instance.bundle];
		const float time = bundle.playBackward ? bundle.length - instance.time : instance.time;
		if (instance.strafe >= 0 && bundle.clips.size() == 4) {
			// addAnimation puts each clip first: index i is the i-th from the end.
			const Clip& strafe = m_system.clips()[bundle.clips[3 - static_cast<size_t>(instance.strafe)].clip];
			const Clip& along = m_system.clips()[bundle.clips[3 - static_cast<size_t>(instance.forwardOrBack)].clip];
			const Animation* a = m_systems.animation(strafe.path);
			const Animation* b = m_systems.animation(along.path);
			if (!a || !b) {
				continue;
			}
			const float fa = time / m_systems.clipLength(strafe);
			const float fb = time / m_systems.clipLength(along);
			for (size_t node = 0; node < skeleton.nodes.size(); ++node) {
				Quaternion ra, rb;
				Position pa, pb;
				const bool hasA = sampleNode(*a, node, fa, strafe.looping, ra, pa);
				const bool hasB = sampleNode(*b, node, fb, along.looping, rb, pb);
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
				blendInto(node, slerp(ra, rb, instance.blend), p, weight);
			}
			continue;
		}
		for (const Bundle::Entry& entry : bundle.clips) {
			if (time < entry.startTime) {
				continue;
			}
			const Clip& clip = m_system.clips()[entry.clip];
			const Animation* animation = m_systems.animation(clip.path);
			if (!animation) {
				continue;
			}
			const float local = time - entry.startTime;
			float clipWeight = weight;
			if (clip.fadeInTime > 0 && local < clip.fadeInTime) {
				clipWeight *= local / clip.fadeInTime;
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
					blendInto(node, r, p, clipWeight);
				}
			}
		}
	}
}

} // namespace dice::hfe::anim
