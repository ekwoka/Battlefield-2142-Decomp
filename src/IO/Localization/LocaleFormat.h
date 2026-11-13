// Dates, times and numbers in the game's language, as the menus write them.
//
// SwiffPlayer.dll (0x10007730) and RendDX9 (0x1017af30, for SwiffHost's
// progress chart, 0x10172af0 / 0x10172c20) carry the same table, built
// once: the language name to a Windows LCID (English 0x409, French 0x40c,
// German 0x407, Italian 0x410, Russian 0x419, Swedish 0x41d, Danish 0x406,
// Finnish 0x40b, Spanish 0x40a, Dutch 0x413, Polish 0x415, Czech 0x405,
// Korean 0x412, Chinese 0xc04, Japanese 0x411, Thai 0x41e). The language
// looked up is SwiffHost slot 0x3a8's (0x0046c510: the profile's
// GSLanguage, "English" without one); a name not in the table (the game's
// own Chinese_Simplified and Chinese_Traditional among them: the table says
// "Chinese") is added by the map's operator[] with LCID 0, which Windows
// reads as the user's default locale.
//
// The LCID goes to GetDateFormatA / GetTimeFormatA / GetNumberFormatA with
// no format of their own, so each writes the locale's defaults:
//   - 0x10007b70: the short date (LOCALE_SSHORTDATE) of a time_t, "" for 0;
//   - 0x10007bf0: the time (LOCALE_STIMEFORMAT), without the seconds
//     (TIME_NOSECONDS) when its flag is clear, as every caller passes it;
//   - 0x10007aa0: the two joined by a space, the time with its seconds,
//     "" for 0;
//   - 0x10007c60: a number's text ("%.2f" at every caller) with the
//     locale's decimal and thousands separators, grouping by 3, two
//     decimals (LOCALE_IDIGITS), a leading zero and a leading minus.
// The rewrite has no Windows locale database: each LCID's defaults are
// Windows XP's (flag 0x80000000, LOCALE_NOUSEROVERRIDE, keeps the user's
// settings out of all but the time), kept here; LCID 0 (the user's
// default locale) takes English's. Text is UTF-8 (the separators' no-break
// space U+00A0, Korean's and Japanese's day halves).
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace dice::hfe::io {

// 0x10007730 / RendDX9 0x1017af30: 0 for a language not in the table.
uint32_t languageLcid(const std::string& language);

// 0x10007c60: GetNumberFormatA(lcid, 0x80000000, value, NULL). A value that
// isn't a number (GetNumberFormatA fails) comes back as it was.
std::string formatNumber(const std::string& value, uint32_t lcid);
// 0x10007b70: GetDateFormatA's short date in the local time, "" for 0.
std::string formatDate(std::time_t time, uint32_t lcid);
// 0x10007bf0: GetTimeFormatA in the local time, with the seconds only when
// `seconds` (its third argument) is set.
std::string formatTime(std::time_t time, uint32_t lcid, bool seconds);
// 0x10007aa0: the date, a space and the time with seconds; "" for 0.
std::string formatDateTime(std::time_t time, uint32_t lcid);

// The same on a broken-down local time (tests).
std::string formatDate(const std::tm& local, uint32_t lcid);
std::string formatTime(const std::tm& local, uint32_t lcid, bool seconds);

} // namespace dice::hfe::io
