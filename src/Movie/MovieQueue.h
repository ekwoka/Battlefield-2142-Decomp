// The engine's movie queue (bf2Engine.playMovie; BF2142.exe's main loop
// object, the list at +0x100):
//
//   - playMovie <file> <mode> (0x004082a0) appends a movie: nothing on a
//     dedicated server (0x0042b800); "Movies\legal.bik" becomes
//     "Movies\legal_na.bik" for a US or Canadian English locale
//     (0x00403230: LOCALE 0x409 or 0x1009);
//   - when no movie plays, the next is taken off the front (0x00408680)
//     and opened by an FSMoviePlayer (RendDX9's, "dice.hfe.geom.
//     FSMoviePlayer"); one that won't open is dropped for the next;
//   - while one plays (0x0040bd90): the cursor is hidden, a frame drawn
//     each pass, and it ends when it is over, or on Esc (key 1) unless
//     its mode is 1 (+0x10c != 1: the logos are mode 1, the intro 0);
//   - the menu's idle "screensaver" (Movies\screensaver.bik, not shipped)
//     ends on any key.
//
// Init.con queues the start-up ones: health.bik and warning.bik (not
// shipped), EA, dice and legal (mode 1), intro (mode 0).
#pragma once

#include <deque>
#include <string>

namespace dice::hfe::movie {

struct QueuedMovie {
	std::string file; // as given ("Movies\\EA.bik"), relative to the mod
	int mode = 0;     // 1: can't be skipped
	bool skippable() const { return mode != 1; }
};

class MovieQueue {
public:
	// A dedicated server queues nothing.
	bool dedicated = false;
	// The user's locale is US or Canadian English (legal_na.bik).
	bool northAmerica = false;

	void playMovie(const std::string& file, int mode);
	bool empty() const { return m_movies.empty(); }
	size_t size() const { return m_movies.size(); }
	QueuedMovie takeNext();
	void clear() { m_movies.clear(); }
	const std::deque<QueuedMovie>& movies() const { return m_movies; }

	// Whether the user's locale (LC_ALL, LC_MESSAGES, LANG; the system's
	// on Windows) is en_US or en_CA, as 0x00403230 asks Windows.
	static bool userInNorthAmerica();

private:
	std::deque<QueuedMovie> m_movies;
};

} // namespace dice::hfe::movie
