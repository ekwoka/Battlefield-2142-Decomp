#include "Scroller.h"

#include "Draw.h"
#include "Hud/HudFont.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ui {

namespace {

constexpr int kSlots = 40;
constexpr float kFirstY = 15360; // twips: 768 px
constexpr float kStep = 400;     // twips: 20 px

// UTF-8 when it is valid, else Latin-1 (credits.con's own names are).
std::vector<uint32_t> decode(const std::string& text)
{
	std::vector<uint32_t> out;
	bool valid = true;
	for (size_t i = 0; i < text.size() && valid;) {
		const auto c = static_cast<unsigned char>(text[i]);
		const int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
		if (extra < 0 || i + static_cast<size_t>(extra) >= text.size() + (extra == 0 ? 1 : 0)) {
			valid = false;
			break;
		}
		uint32_t code = extra == 0 ? c : c & (0x3f >> extra);
		for (int k = 1; k <= extra; ++k) {
			const auto next = static_cast<unsigned char>(text[i + static_cast<size_t>(k)]);
			if ((next & 0xc0) != 0x80) {
				valid = false;
			}
			code = (code << 6) | (next & 0x3f);
		}
		out.push_back(code);
		i += static_cast<size_t>(extra) + 1;
	}
	if (!valid) {
		out.clear();
		for (const char c : text) {
			out.push_back(static_cast<unsigned char>(c));
		}
	}
	return out;
}

} // namespace

void CreditsScroller::addCredit(const std::string& text, int size)
{
	m_credits.push_back({text, size});
	m_built = false;
}

void CreditsScroller::clearCredits()
{
	m_credits.clear();
	m_built = false;
}

void CreditsScroller::setSpeed(float twipsPerSecond)
{
	m_speed = std::clamp(twipsPerSecond, 500.0f, 4000.0f);
}

void CreditsScroller::setRect(int left, int top, int right, int bottom)
{
	m_rect[0] = left;
	m_rect[1] = top;
	m_rect[2] = right;
	m_rect[3] = bottom;
}

void CreditsScroller::enable(bool on)
{
	m_enabled = on;
	m_index = 0;
	m_reset = true;
	// 0x10177210 (on the original's next frame, the reset flag): the slots
	// from the first line, that frame standing still.
	m_slots.clear();
	if (on && !m_credits.empty()) {
		for (int i = 0; i < kSlots; ++i) {
			m_slots.push_back({nextLine(), kFirstY + kStep * static_cast<float>(i)});
		}
	}
	m_reset = false;
}

size_t CreditsScroller::nextLine()
{
	// 0x10177190: the next line, the first again after the last.
	if (m_index >= m_credits.size()) {
		m_index = 0;
	}
	return m_index++;
}

void CreditsScroller::addTime(double seconds)
{
	if (!m_enabled || m_slots.empty()) {
		return;
	}
	// 0x10177290: up by the speed; the top slot gone above -400 twips, a
	// new one 400 below the last.
	for (Slot& slot : m_slots) {
		slot.y -= m_speed * static_cast<float>(seconds);
	}
	while (m_slots.front().y < -kStep) {
		const float last = m_slots.back().y;
		m_slots.erase(m_slots.begin());
		m_slots.push_back({nextLine(), last + kStep});
	}
}

void CreditsScroller::draw(Drawer& drawer, const std::function<std::string(const std::string&)>& localize, std::vector<MenuBatch>& out)
{
	if (!m_enabled) {
		return;
	}
	if (!m_built) {
		m_lines.clear();
		for (const Credit& credit : m_credits) {
			Line line;
			line.size = credit.size;
			line.codes = decode(credit.text.empty() || !localize ? credit.text : localize(credit.text));
			m_lines.push_back(std::move(line));
		}
		m_built = true;
	}
	if (m_lines.empty()) {
		return;
	}
	const size_t first = out.size();
	for (const Slot& slot : m_slots) {
		if (slot.line >= m_lines.size()) {
			continue;
		}
		const Line& line = m_lines[slot.line];
		if (line.codes.empty()) {
			continue;
		}
		const hud::HudFont* font = line.size == 2 ? drawer.engineFont("StandardTextBold", 15)
			: line.size == 1                      ? drawer.engineFont("StandardTextBold", 13)
												  : drawer.engineFont("DynamicText", 11);
		if (!font) {
			continue;
		}
		// Centred in the box W * 1199 / 1024 wide (0x10183570, alignment 2).
		const float width = drawer.lineWidth(*font, line.codes);
		const float box = 1024.0f * 1199.0f / 1024.0f;
		const float x = std::floor(std::max(0.0f, box - width) / 2);
		drawer.engineLine(*font, line.codes, x, std::nearbyint(slot.y * 0.05f + 0.5f), {255, 255, 255, 255}, out);
	}
	// The scissor (0x10117910).
	clipBatches(out, first, static_cast<float>(m_rect[0]), static_cast<float>(m_rect[1]), static_cast<float>(m_rect[2]),
		static_cast<float>(m_rect[3]));
}

} // namespace dice::hfe::ui
