#include "MovieQueue.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dice::hfe::movie {

namespace {

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

} // namespace

void MovieQueue::playMovie(const std::string& file, int mode)
{
	if (dedicated) {
		return; // "These should be noops in server & editor" (Init.con)
	}
	std::string name = file;
	std::string normal = lower(file);
	std::replace(normal.begin(), normal.end(), '/', '\\');
	if (northAmerica && normal == "movies\\legal.bik") {
		name = "Movies\\legal_na.bik";
	}
	m_movies.push_back({name, mode});
}

QueuedMovie MovieQueue::takeNext()
{
	if (m_movies.empty()) {
		return {};
	}
	QueuedMovie next = m_movies.front();
	m_movies.pop_front();
	return next;
}

bool MovieQueue::userInNorthAmerica()
{
#ifdef _WIN32
	// 0x00403230: GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_ILANGUAGE).
	char buffer[16] = {};
	GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_ILANGUAGE, buffer, sizeof buffer);
	const long id = std::strtol(buffer, nullptr, 16);
	return id == 0x409 || id == 0x1009;
#else
	for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
		const char* value = std::getenv(name);
		if (value && *value) {
			const std::string locale = lower(value);
			return locale.rfind("en_us", 0) == 0 || locale.rfind("en_ca", 0) == 0;
		}
	}
	return false;
#endif
}

} // namespace dice::hfe::movie
