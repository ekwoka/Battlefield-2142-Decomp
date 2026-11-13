#include "LocaleFormat.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace dice::hfe::io {

namespace {

// A locale's defaults (Windows XP's NLS data for the LCID).
struct Locale {
	uint32_t lcid;
	const char* shortDate;  // LOCALE_SSHORTDATE
	const char* timeFormat; // LOCALE_STIMEFORMAT
	const char* am;         // LOCALE_S1159
	const char* pm;         // LOCALE_S2359
	const char* decimal;    // LOCALE_SDECIMAL
	const char* thousand;   // LOCALE_STHOUSAND ("3;0" grouping, LOCALE_IDIGITS 2,
	                        // LOCALE_ILZERO 1, LOCALE_INEGNUMBER 1 for all of them)
	int yearOffset;         // the default calendar's: Thai's Buddhist era
};

constexpr const char* kNbsp = "\xc2\xa0"; // U+00A0

const Locale kLocales[] = {
	{0x409, "M/d/yyyy", "h:mm:ss tt", "AM", "PM", ".", ",", 0},           // English (United States)
	{0x40c, "dd/MM/yyyy", "HH:mm:ss", "", "", ",", kNbsp, 0},             // French (France)
	{0x407, "dd.MM.yyyy", "HH:mm:ss", "", "", ",", ".", 0},               // German (Germany)
	{0x410, "dd/MM/yyyy", "H.mm.ss", "", "", ",", ".", 0},                // Italian (Italy)
	{0x419, "dd.MM.yyyy", "H:mm:ss", "", "", ",", kNbsp, 0},              // Russian
	{0x41d, "yyyy-MM-dd", "HH:mm:ss", "", "", ",", kNbsp, 0},             // Swedish
	{0x406, "dd-MM-yyyy", "HH:mm:ss", "", "", ",", ".", 0},               // Danish
	{0x40b, "d.M.yyyy", "H:mm:ss", "", "", ",", kNbsp, 0},                // Finnish
	{0x40a, "dd/MM/yyyy", "H:mm:ss", "", "", ",", ".", 0},                // Spanish (traditional sort)
	{0x413, "d-M-yyyy", "H:mm:ss", "", "", ",", ".", 0},                  // Dutch (Netherlands)
	{0x415, "yyyy-MM-dd", "HH:mm:ss", "", "", ",", kNbsp, 0},             // Polish
	{0x405, "d.M.yyyy", "H:mm:ss", "", "", ",", kNbsp, 0},                // Czech
	{0x412, "yyyy-MM-dd", "tt h:mm:ss", "\xec\x98\xa4\xec\xa0\x84", "\xec\x98\xa4\xed\x9b\x84", ".", ",", 0}, // Korean
	{0xc04, "d/M/yyyy", "H:mm:ss", "\xe4\xb8\x8a\xe5\x8d\x88", "\xe4\xb8\x8b\xe5\x8d\x88", ".", ",", 0},     // Chinese (Hong Kong)
	{0x411, "yyyy/MM/dd", "H:mm:ss", "\xe5\x8d\x88\xe5\x89\x8d", "\xe5\x8d\x88\xe5\xbe\x8c", ".", ",", 0},   // Japanese
	{0x41e, "d/M/yyyy", "H:mm:ss", "AM", "PM", ".", ",", 543},            // Thai (Buddhist calendar)
};

const Locale& localeOf(uint32_t lcid)
{
	for (const Locale& locale : kLocales) {
		if (locale.lcid == lcid) {
			return locale;
		}
	}
	return kLocales[0]; // 0, the user's default locale: English's here
}

std::string number(int value, int width)
{
	std::string text = std::to_string(value);
	while (static_cast<int>(text.size()) < width) {
		text.insert(text.begin(), '0');
	}
	return text;
}

// A run of one picture letter: its length.
size_t run(const std::string& picture, size_t at)
{
	size_t end = at;
	while (end < picture.size() && picture[end] == picture[at]) {
		++end;
	}
	return end - at;
}

} // namespace

uint32_t languageLcid(const std::string& language)
{
	static const std::map<std::string, uint32_t> kTable {
		{"English", 0x409}, {"French", 0x40c}, {"German", 0x407}, {"Italian", 0x410}, {"Russian", 0x419},
		{"Swedish", 0x41d}, {"Danish", 0x406}, {"Finnish", 0x40b}, {"Spanish", 0x40a}, {"Dutch", 0x413},
		{"Polish", 0x415}, {"Czech", 0x405}, {"Korean", 0x412}, {"Chinese", 0xc04}, {"Japanese", 0x411},
		{"Thai", 0x41e}};
	const auto it = kTable.find(language); // as written (std::map<std::string>)
	return it == kTable.end() ? 0 : it->second;
}

std::string formatNumber(const std::string& value, uint32_t lcid)
{
	// GetNumberFormatA's input: an optional minus, digits, an optional
	// '.' and digits; anything else fails.
	size_t at = 0;
	const bool negative = !value.empty() && value[0] == '-';
	at += negative ? 1 : 0;
	std::string whole, fraction;
	while (at < value.size() && std::isdigit(static_cast<unsigned char>(value[at]))) {
		whole += value[at++];
	}
	if (at < value.size() && value[at] == '.') {
		++at;
		while (at < value.size() && std::isdigit(static_cast<unsigned char>(value[at]))) {
			fraction += value[at++];
		}
	}
	if (at != value.size() || (whole.empty() && fraction.empty())) {
		return value;
	}
	// Two decimals, rounded half away from zero on the digits.
	constexpr size_t kDigits = 2;
	std::string digits = whole + fraction.substr(0, std::min(fraction.size(), kDigits));
	digits.append(kDigits - std::min(fraction.size(), kDigits), '0');
	if (fraction.size() > kDigits && fraction[kDigits] >= '5') {
		size_t i = digits.size();
		while (i > 0) {
			--i;
			if (digits[i] == '9') {
				digits[i] = '0';
				continue;
			}
			++digits[i];
			break;
		}
		if (i == 0 && digits[0] == '0') {
			digits.insert(digits.begin(), '1');
		}
	}
	std::string integer = digits.substr(0, digits.size() - kDigits);
	const std::string decimals = digits.substr(digits.size() - kDigits);
	while (integer.size() > 1 && integer[0] == '0') {
		integer.erase(integer.begin());
	}
	if (integer.empty()) {
		integer = "0"; // LOCALE_ILZERO 1
	}
	const Locale& locale = localeOf(lcid);
	std::string grouped;
	for (size_t i = 0; i < integer.size(); ++i) {
		if (i > 0 && (integer.size() - i) % 3 == 0) {
			grouped += locale.thousand;
		}
		grouped += integer[i];
	}
	const bool zero = digits.find_first_not_of('0') == std::string::npos;
	return (negative && !zero ? "-" : "") + grouped + locale.decimal + decimals;
}

std::string formatDate(const std::tm& local, uint32_t lcid)
{
	const Locale& locale = localeOf(lcid);
	const std::string picture = locale.shortDate;
	std::string out;
	for (size_t at = 0; at < picture.size();) {
		const char c = picture[at];
		const size_t n = run(picture, at);
		if (c == 'd') {
			out += number(local.tm_mday, n >= 2 ? 2 : 1);
		} else if (c == 'M') {
			out += number(local.tm_mon + 1, n >= 2 ? 2 : 1);
		} else if (c == 'y') {
			const int year = local.tm_year + 1900 + locale.yearOffset;
			out += n >= 3 ? number(year, 4) : number(year % 100, n >= 2 ? 2 : 1);
		} else {
			out.append(n, c);
		}
		at += n;
	}
	return out;
}

std::string formatTime(const std::tm& local, uint32_t lcid, bool seconds)
{
	const Locale& locale = localeOf(lcid);
	std::string picture = locale.timeFormat;
	if (!seconds) {
		// TIME_NOSECONDS: the seconds and what stands between them and the
		// element before.
		const size_t s = picture.find('s');
		if (s != std::string::npos) {
			size_t from = s;
			while (from > 0 && std::string("hHmt").find(picture[from - 1]) == std::string::npos) {
				--from;
			}
			picture.erase(from, run(picture, s) + (s - from));
		}
	}
	std::string out;
	for (size_t at = 0; at < picture.size();) {
		const char c = picture[at];
		const size_t n = run(picture, at);
		const int width = n >= 2 ? 2 : 1;
		if (c == 'h') {
			const int hour = local.tm_hour % 12;
			out += number(hour == 0 ? 12 : hour, width);
		} else if (c == 'H') {
			out += number(local.tm_hour, width);
		} else if (c == 'm') {
			out += number(local.tm_min, width);
		} else if (c == 's') {
			out += number(local.tm_sec, width);
		} else if (c == 't') {
			out += local.tm_hour < 12 ? locale.am : locale.pm;
		} else {
			out.append(n, c);
		}
		at += n;
	}
	return out;
}

std::string formatDate(std::time_t time, uint32_t lcid)
{
	if (time == 0) {
		return std::string();
	}
	const std::tm* local = std::localtime(&time);
	return local ? formatDate(*local, lcid) : std::string();
}

std::string formatTime(std::time_t time, uint32_t lcid, bool seconds)
{
	const std::tm* local = std::localtime(&time);
	return local ? formatTime(*local, lcid, seconds) : std::string();
}

std::string formatDateTime(std::time_t time, uint32_t lcid)
{
	if (time == 0) {
		return std::string();
	}
	const std::tm* local = std::localtime(&time);
	if (!local) {
		return std::string();
	}
	const std::tm copy = *local;
	return formatDate(copy, lcid) + " " + formatTime(copy, lcid, true);
}

} // namespace dice::hfe::io
