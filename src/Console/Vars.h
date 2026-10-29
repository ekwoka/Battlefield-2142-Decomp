// The engine's settings table ("vars", DAT_00a1b5cc): named values the
// code registers with a default (0x004238a0 for floats, 0x004237c0 for
// ints, 0 unless a script sets them) and the scripts set with
// "vars.set <name> <value>" (Vars.set, 0x004244c1), Settings/Sound.con's
// sound-* and engine-audio-* among them. Names are case-insensitive.
// Readers ask each time (the exe keeps a pointer to the value), so a
// script's change is seen at once.
#pragma once

#include <cstdlib>
#include <map>
#include <string>

namespace dice::hfe {

class Vars {
public:
	static Vars& global()
	{
		static Vars vars;
		return vars;
	}

	void set(const std::string& name, const std::string& value) { m_values[lower(name)] = value; }
	bool has(const std::string& name) const { return m_values.count(lower(name)) != 0; }
	std::string get(const std::string& name) const
	{
		const auto it = m_values.find(lower(name));
		return it == m_values.end() ? std::string() : it->second;
	}
	// The value as a number, `fallback` (the registered default) when unset.
	float number(const std::string& name, float fallback) const
	{
		const auto it = m_values.find(lower(name));
		return it == m_values.end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
	}
	void clear() { m_values.clear(); }

private:
	static std::string lower(std::string s)
	{
		for (char& c : s) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}
		return s;
	}
	std::map<std::string, std::string> m_values;
};

} // namespace dice::hfe
