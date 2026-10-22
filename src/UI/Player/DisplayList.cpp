#include "DisplayList.h"

#include "Draw.h"
#include "UI/Avm1/Object.h"

#include <algorithm>

namespace dice::hfe::ui {

namespace {

enum TagCode : uint16_t {
	kEnd = 0,
	kShowFrame = 1,
	kPlaceObject = 4,
	kRemoveObject = 5,
	kDoAction = 12,
	kPlaceObject2 = 26,
	kRemoveObject2 = 28,
	kFrameLabel = 43,
};

} // namespace

Timeline Timeline::fromTags(const std::vector<SwfTag>& tags, const std::vector<uint8_t>& swf)
{
	Timeline timeline;
	Frame current;
	bool pending = false;
	for (const SwfTag& tag : tags) {
		switch (tag.code) {
		case kShowFrame:
			timeline.frames.push_back(std::move(current));
			current = Frame();
			pending = false;
			break;
		case kDoAction:
			current.actions.push_back({tag.offset, tag.length});
			pending = true;
			break;
		case kFrameLabel: {
			const size_t end = std::min(tag.offset + tag.length, swf.size());
			std::string label;
			for (size_t i = tag.offset; i < end && swf[i]; ++i) {
				label += static_cast<char>(swf[i]);
			}
			current.label = label;
			timeline.labels[label] = static_cast<int>(timeline.frames.size());
			pending = true;
			break;
		}
		case kPlaceObject:
		case kPlaceObject2:
		case kRemoveObject:
		case kRemoveObject2:
			current.controls.push_back(tag);
			pending = true;
			break;
		default:
			break; // definitions are the dictionary's
		}
	}
	if (pending) {
		timeline.frames.push_back(std::move(current)); // a last frame without ShowFrame
	}
	if (timeline.frames.empty()) {
		timeline.frames.emplace_back();
	}
	return timeline;
}

MovieClip::MovieClip(const swf::CharacterDictionary& dictionary, const std::vector<uint8_t>& swf, const Timeline& timeline,
	uint16_t characterId, MovieClip* parent)
	: m_dictionary(dictionary), m_swf(swf), m_timeline(timeline), m_characterId(characterId), m_parent(parent)
{
	if (parent) {
		m_timelines = parent->m_timelines;
	} else {
		m_ownTimelines = std::make_unique<std::map<uint16_t, std::unique_ptr<Timeline>>>();
		m_timelines = m_ownTimelines.get();
	}
}

int MovieClip::frameOfLabel(const std::string& label) const
{
	const auto it = m_timeline.labels.find(label);
	return it != m_timeline.labels.end() ? it->second : -1;
}

Instance* MovieClip::instanceNamed(const std::string& name)
{
	for (auto& [depth, instance] : m_displayList) {
		if (avm1::PropertyMap::key(instance.name) == avm1::PropertyMap::key(name)) {
			return &instance;
		}
	}
	return nullptr;
}

void MovieClip::remove(uint16_t depth) { m_displayList.erase(depth); }

// PlaceObject2: flags (clip actions, clip depth, name, ratio, colour
// transform, matrix, character, move), depth, then what the flags say.
PlaceRecord parsePlaceObject(const SwfTag& tag, const std::vector<uint8_t>& swf)
{
	PlaceRecord r;
	swf::BitReader in(swf.data() + tag.offset, tag.length);
	if (tag.code == kPlaceObject) {
		r.hasCharacter = true;
		r.character = in.u16();
		r.depth = in.u16();
		r.hasMatrix = true;
		r.matrix = in.matrix();
		if (in.remaining() > 0) {
			r.hasColour = true;
			r.colour = in.cxform(false);
		}
		return r;
	}
	const uint8_t flags = in.u8();
	r.depth = in.u16();
	r.move = flags & 0x01;
	if (flags & 0x02) {
		r.hasCharacter = true;
		r.character = in.u16();
	}
	if (flags & 0x04) {
		r.hasMatrix = true;
		r.matrix = in.matrix();
	}
	if (flags & 0x08) {
		r.hasColour = true;
		r.colour = in.cxform(true);
	}
	if (flags & 0x10) {
		r.hasRatio = true;
		r.ratio = in.u16() / 65535.0f;
	}
	if (flags & 0x20) {
		r.hasName = true;
		r.name = in.string();
	}
	if (flags & 0x40) {
		r.hasClipDepth = true;
		r.clipDepth = in.u16();
	}
	if (flags & 0x80) {
		r.hasEvents = true;
		in.u16(); // reserved
		in.u32(); // all the events' bits
		for (;;) {
			const uint32_t events = in.u32();
			if (events == 0 || !in.ok()) {
				break;
			}
			uint32_t size = in.u32();
			ClipEvent event;
			event.events = events;
			if (events & kClipEventKeyPress) {
				event.keyCode = in.u8();
				--size;
			}
			event.actions = {tag.offset + in.position(), size};
			in.seek(in.position() + size);
			r.events.push_back(event);
		}
	}
	return r;
}

uint16_t removedDepth(const SwfTag& tag, const std::vector<uint8_t>& swf)
{
	swf::BitReader in(swf.data() + tag.offset, tag.length);
	if (tag.code == kRemoveObject) {
		in.u16(); // the character
	}
	return in.u16();
}

void MovieClip::place(const SwfTag& tag)
{
	const PlaceRecord r = parsePlaceObject(tag, m_swf);
	const bool move = r.move;
	Instance* instance = nullptr;
	if (r.hasCharacter) {
		auto existing = m_displayList.find(r.depth);
		// Replacing a different character (or placing anew) starts fresh;
		// the same character keeps its state (a clip keeps playing).
		if (existing == m_displayList.end() || !move || existing->second.character != r.character) {
			Instance fresh;
			fresh.character = r.character;
			fresh.depth = r.depth;
			if (existing != m_displayList.end() && move) {
				fresh.matrix = existing->second.matrix;
				fresh.colour = existing->second.colour;
				fresh.name = existing->second.name;
			}
			if (const swf::Sprite* sprite = m_dictionary.sprite(r.character)) {
				auto& timeline = (*m_timelines)[r.character];
				if (!timeline) {
					timeline = std::make_unique<Timeline>(Timeline::fromTags(sprite->tags, m_swf));
				}
				fresh.clip = std::make_unique<MovieClip>(m_dictionary, m_swf, *timeline, r.character, this);
			}
			if (const swf::EditText* field = m_dictionary.editText(r.character)) {
				fresh.text = field->initialText;
			}
			m_displayList[r.depth] = std::move(fresh);
		}
		instance = &m_displayList[r.depth];
	} else {
		auto existing = m_displayList.find(r.depth);
		if (existing == m_displayList.end()) {
			return; // moving nothing
		}
		instance = &existing->second;
	}
	if (r.hasMatrix) {
		instance->matrix = r.matrix;
	}
	if (r.hasColour) {
		instance->colour = r.colour;
	}
	if (r.hasRatio) {
		instance->ratio = r.ratio;
	}
	if (r.hasName) {
		instance->name = r.name;
	}
	if (r.hasClipDepth) {
		instance->clipDepth = r.clipDepth;
	}
	if (r.hasEvents) {
		instance->events = r.events;
	}
}

void MovieClip::enterFrame(int frame, std::vector<std::pair<MovieClip*, ActionBlock>>& actions, bool runActions)
{
	m_frame = frame;
	const Frame& f = m_timeline.frames[static_cast<size_t>(frame)];
	for (const SwfTag& tag : f.controls) {
		if (tag.code == kRemoveObject2 || tag.code == kRemoveObject) {
			remove(removedDepth(tag, m_swf));
		} else {
			place(tag);
		}
	}
	if (runActions) {
		for (const ActionBlock& block : f.actions) {
			actions.emplace_back(this, block);
		}
	}
}

void MovieClip::gotoFrame(int frame, std::vector<std::pair<MovieClip*, ActionBlock>>& actions)
{
	frame = std::clamp(frame, 0, frameCount() - 1);
	if (frame == m_frame) {
		return;
	}
	if (frame < m_frame) {
		// Back: replay from the start; only what stays across keeps its state.
		std::map<uint16_t, Instance> before = std::move(m_displayList);
		m_displayList.clear();
		for (int i = 0; i < frame; ++i) {
			enterFrame(i, actions, false);
		}
		for (auto& [depth, instance] : m_displayList) {
			auto old = before.find(depth);
			if (old != before.end() && old->second.character == instance.character && old->second.clip) {
				instance.clip = std::move(old->second.clip); // the same clip goes on
			}
		}
		m_frame = frame - 1;
	} else {
		// Forward: the frames between are entered without their actions.
		for (int i = m_frame + 1; i < frame; ++i) {
			enterFrame(i, actions, false);
		}
	}
	enterFrame(frame, actions, true);
}

void MovieClip::advance(std::vector<std::pair<MovieClip*, ActionBlock>>& actions)
{
	if (m_frame < 0) {
		enterFrame(0, actions, true);
	} else if (m_playing && frameCount() > 1) {
		const int next = m_frame + 1 < frameCount() ? m_frame + 1 : 0;
		if (next == 0) {
			gotoFrame(0, actions);
		} else {
			enterFrame(next, actions, true);
		}
	}
	for (auto& [depth, instance] : m_displayList) {
		if (instance.clip) {
			instance.clip->advance(actions);
		}
	}
}

void MovieClip::draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out) const
{
	for (const auto& [depth, instance] : m_displayList) {
		if (!instance.visible || instance.clipDepth != 0) {
			continue; // masks are not drawn (nor applied yet)
		}
		const swf::Matrix at = multiply(world, instance.matrix);
		const swf::ColorTransform tint = combine(colour, instance.colour);
		if (instance.clip) {
			instance.clip->draw(drawer, at, tint, out);
		} else if (m_dictionary.shape(instance.character)) {
			drawer.shape(instance.character, at, tint, out);
		} else if (m_dictionary.morphShape(instance.character)) {
			drawer.morphShape(instance.character, instance.ratio, at, tint, out);
		} else if (m_dictionary.editText(instance.character)) {
			drawer.editText(instance.character, instance.text, at, tint, out);
		} else if (const swf::Button* button = m_dictionary.button(instance.character)) {
			for (const swf::ButtonRecord& record : button->records) {
				if (!(record.states & instance.buttonState)) {
					continue;
				}
				const swf::Matrix part = multiply(at, record.matrix);
				const swf::ColorTransform partTint = combine(tint, record.colorTransform);
				if (m_dictionary.shape(record.characterId)) {
					drawer.shape(record.characterId, part, partTint, out);
				} else if (m_dictionary.editText(record.characterId)) {
					drawer.editText(record.characterId, m_dictionary.editText(record.characterId)->initialText, part, partTint, out);
				}
			}
		}
	}
}

} // namespace dice::hfe::ui
