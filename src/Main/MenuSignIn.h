// The shipped main menu's login, played through by the game itself for a
// level started from the command line (--menu with +level), so that the
// game it starts is a logged-in soldier's, as one started from the
// singleplayer page is: the soldier's profile, settings, unlocks and stats,
// and an in-game menu (Esc) past its login page.
#pragma once

#include <string>

namespace dice::hfe::ui {
class Player;
}

namespace dice::hfe::menu {

class MenuBindings;

// Logs in on the login page as a player would: the account name typed when
// the page has none, LOGIN, OK to the "no connection" popup (offline
// play), then the account's first soldier and LOGIN again; done once the
// main page shows. The movie runs headless, its jobs (the online check,
// the login) in real time, for at most `seconds`. An account that wants a
// password online isn't logged in: the page stays as it is. True once
// logged in to a soldier with the main page up.
bool signInForDirectStart(ui::Player& menu, MenuBindings& bindings, const std::string& accountName, double seconds = 20);

// The main page's SINGLEPLAY, for an in-game menu (Esc) on the page a game
// started from it shows: its DISCONNECT once the game runs
// (MenuGameState.h). True once the page is up.
bool showSingleplayPage(ui::Player& menu);

} // namespace dice::hfe::menu
