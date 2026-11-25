// enhanced.singleplayBotCount: a players slider on the singleplayer page.
//
// The page (mainMenu.dfm's sprite 913, `singleplay.mc_singleplayer`) is
// retail's: its frame script defines startServer, which sets
// GeneralSettings.setMaxBots(16), ServerSettings.setMaxPlayers(16) and
// setMaxBotsIncludeHumans(true) before Multiplay.createServer, for its
// START PLAYLIST button and for instant action alike, so every map plays
// with 16 players, you included. Its bot skill is a dice.UIs.Forms.Slider
// (linkage "Slider", char 702) placed as mcBotSkill with
// callBackFunctionSpecial "adjustBotSkill", a TextContainer header
// (HEADER_botskill) above it and TextContainers for its value.
//
// With the option on, the page gets the same pieces beside the bot skill,
// attached as the movie's own script would (attachMovie of "Slider" and
// "TextContainer" with the authored instances' init values, right of the
// bot skill row over START PLAYLIST): the header (the create-server page's
// MULTIPLAY_3_maxplayers, "MAX PLAYERS"), the slider mcBotCount (1..64
// players, its callback adjustBotCount on the page) and its number.
// The target is MenuBindings::singleplayPlayers (saved in the profile).
// The page's startServer is wrapped so that its createServer takes the
// target (MenuBindings::setSingleplayStarting; the create-server page's
// createServer doesn't). Off again, the pieces go and startServer is the
// page's own. As updateMenuGameState (MenuGameState.h) does for the
// page's buttons, this reaches into the page from outside its scripts.
#pragma once

namespace dice::hfe::ui {
class Player;
}

namespace dice::hfe::menu {

class MenuBindings;

// Adds or removes the control to match bindings.singleplayBotCount; does
// nothing while the page isn't there. Cheap enough for every menu tick
// (Player::setTickHook).
void updateSingleplayBotCount(ui::Player& player, MenuBindings& bindings);

} // namespace dice::hfe::menu
