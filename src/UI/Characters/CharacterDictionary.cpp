#include "CharacterDictionary.h"

namespace dice::hfe::ui::swf {

void CharacterDictionary::warn(const SwfTag& tag, const std::string& message)
{
	m_warnings.push_back(std::string(swfTagName(tag.code)) + " at " + std::to_string(tag.headerOffset) + ": " + message);
}

const Character* CharacterDictionary::find(uint16_t id) const
{
	const auto it = m_characters.find(id);
	return it == m_characters.end() ? nullptr : &it->second;
}

uint16_t CharacterDictionary::exportedId(const std::string& name) const
{
	const auto it = m_exports.find(name);
	return it == m_exports.end() ? 0 : it->second;
}

bool CharacterDictionary::load(const std::vector<uint8_t>& swf, const SwfMovie& movie, std::string* error)
{
	m_characters.clear();
	m_exports.clear();
	m_warnings.clear();
	std::vector<uint8_t> jpegTables;

	for (const SwfTag& tag : movie.tags) {
		if (tag.offset + tag.length > swf.size()) {
			if (error) {
				*error = "tag past the end of the movie";
			}
			return false;
		}
		const uint8_t* body = swf.data() + tag.offset;
		const size_t size = tag.length;
		std::string message;
		const auto add = [&](uint16_t id, Character&& c) {
			if (!m_characters.emplace(id, std::move(c)).second) {
				warn(tag, "character " + std::to_string(id) + " defined twice; first kept");
			}
		};
		switch (tag.code) {
		case kTagDefineShape:
		case kTagDefineShape2:
		case kTagDefineShape3: {
			Shape s;
			if (parseDefineShape(body, size, tag.code, s, &message)) {
				add(s.id, std::move(s));
			} else {
				warn(tag, message);
			}
			break;
		}
		case kTagDefineMorphShape: {
			MorphShape m;
			if (parseDefineMorphShape(body, size, m, &message)) {
				add(m.id, std::move(m));
			} else {
				warn(tag, message);
			}
			break;
		}
		case kTagJpegTables:
			jpegTables.assign(body, body + size);
			break;
		case kTagDefineBits:
		case kTagDefineBitsJpeg2:
		case kTagDefineBitsJpeg3:
		case kTagDefineBitsLossless:
		case kTagDefineBitsLossless2: {
			Bitmap b;
			bool ok = false;
			if (tag.code == kTagDefineBits) {
				ok = decodeDefineBits(body, size, jpegTables, b, &message);
			} else if (tag.code == kTagDefineBitsJpeg2) {
				ok = decodeDefineBitsJpeg2(body, size, b, &message);
			} else if (tag.code == kTagDefineBitsJpeg3) {
				ok = decodeDefineBitsJpeg3(body, size, b, &message);
			} else {
				ok = decodeBitsLossless(body, size, tag.code == kTagDefineBitsLossless ? 1 : 2, b, &message);
			}
			if (ok) {
				add(b.id, std::move(b));
			} else {
				warn(tag, message);
			}
			break;
		}
		case kTagDefineFont2: {
			Font f;
			if (parseDefineFont2(body, size, f, &message)) {
				add(f.id, std::move(f));
			} else {
				warn(tag, message);
			}
			break;
		}
		case kTagDefineEditText: {
			EditText t;
			if (parseDefineEditText(body, size, t, &message)) {
				add(t.id, std::move(t));
			} else {
				warn(tag, message);
			}
			break;
		}
		case kTagDefineButton2: {
			Button b;
			if (parseDefineButton2(body, size, tag.offset, b, &message)) {
				add(b.id, std::move(b));
			} else {
				warn(tag, message);
			}
			break;
		}
		case kTagDefineSprite: {
			Sprite s;
			if (size < 4 || !readSpriteTags(swf, tag, s.tags)) {
				warn(tag, "bad sprite tag list");
				break;
			}
			s.id = static_cast<uint16_t>(body[0] | body[1] << 8);
			s.frameCount = static_cast<uint16_t>(body[2] | body[3] << 8);
			s.tag = tag;
			add(s.id, std::move(s));
			break;
		}
		case kTagExportAssets: {
			BitReader in(body, size);
			const uint16_t count = in.u16();
			for (uint16_t i = 0; i < count && in.ok(); ++i) {
				const uint16_t id = in.u16();
				std::string name = in.string();
				if (in.ok()) {
					m_exports[std::move(name)] = id;
				}
			}
			if (!in.ok()) {
				warn(tag, "truncated export list");
			}
			break;
		}
		default:
			break;
		}
	}
	return true;
}

} // namespace dice::hfe::ui::swf
