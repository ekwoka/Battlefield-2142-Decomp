#include "Localization.h"

namespace dice::hfe::io {

namespace {

void appendUtf8(std::string& out, uint32_t c)
{
	if (c < 0x80) {
		out += static_cast<char>(c);
	} else if (c < 0x800) {
		out += static_cast<char>(0xc0 | c >> 6);
		out += static_cast<char>(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		out += static_cast<char>(0xe0 | c >> 12);
		out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
		out += static_cast<char>(0x80 | (c & 0x3f));
	} else {
		out += static_cast<char>(0xf0 | c >> 18);
		out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
		out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
		out += static_cast<char>(0x80 | (c & 0x3f));
	}
}

std::vector<std::string> splitTabs(const std::string& line)
{
	std::vector<std::string> fields;
	size_t start = 0;
	for (;;) {
		const size_t tab = line.find('\t', start);
		fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
		if (tab == std::string::npos) {
			return fields;
		}
		start = tab + 1;
	}
}

// The lexicon's key (0x0065b2c0 -> 0x0065af00): ids are upper-cased when
// stored and when looked up, so lookups ignore case (the menu asks for
// "BUTTON_CREATENEWACCOUNT", the file has "BUTTON_createnewaccount"). Ids
// are ASCII; 0x0065af00's Latin-1 letters (å ä ö ü é) never occur in one.
std::string lexiconKey(std::string id)
{
	for (char& c : id) {
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 'a' + 'A');
		}
	}
	return id;
}

} // namespace

std::string utf16ToUtf8(const uint8_t* data, size_t size)
{
	std::string out;
	out.reserve(size / 2);
	size_t i = size >= 2 && data[0] == 0xff && data[1] == 0xfe ? 2 : 0;
	for (; i + 1 < size; i += 2) {
		uint32_t c = static_cast<uint32_t>(data[i] | data[i + 1] << 8);
		if (c >= 0xd800 && c < 0xdc00 && i + 3 < size) {
			const uint32_t low = static_cast<uint32_t>(data[i + 2] | data[i + 3] << 8);
			if (low >= 0xdc00 && low < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
				i += 2;
			}
		}
		appendUtf8(out, c);
	}
	return out;
}

bool Localization::load(const std::vector<uint8_t>& data, const std::string& language)
{
	const std::string text = utf16ToUtf8(data.data(), data.size());
	size_t column = 0;
	size_t start = 0;
	bool header = true;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = text.substr(start, end - start);
		start = end + 1;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const std::vector<std::string> fields = splitTabs(line);
		if (header) {
			for (size_t i = 1; i < fields.size(); ++i) {
				if (fields[i] == language) {
					column = i;
				}
			}
			if (column == 0) {
				return false;
			}
			header = false;
			continue;
		}
		if (fields.size() <= column || fields[0].empty()) {
			continue;
		}
		std::string value = fields[column];
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
			value = value.substr(1, value.size() - 2);
		}
		// 0x006956f0: an id already there keeps its first text
		// (strings.csv has CREDITS_EA_earedwoodshores twice, in two cases).
		m_strings.emplace(lexiconKey(fields[0]), std::move(value));
	}
	return !header;
}

const std::string* Localization::find(const std::string& id) const
{
	const auto it = m_strings.find(lexiconKey(id));
	return it != m_strings.end() ? &it->second : nullptr;
}

std::string Localization::text(const std::string& id) const
{
	const std::string* found = find(id);
	return found ? *found : id;
}

} // namespace dice::hfe::io
