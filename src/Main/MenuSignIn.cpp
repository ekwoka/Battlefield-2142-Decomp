#include "MenuSignIn.h"

#include "MenuBindings.h"
#include "UI/Player/Player.h"

#include <cctype>
#include <chrono>
#include <thread>

namespace dice::hfe::menu {

namespace {

bool endsWith(const std::string& text, const std::string& end)
{
	return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

// The topmost visible clickable whose target ends so, clicked in its middle.
bool press(ui::Player& menu, const std::string& target)
{
	for (const ui::Player::Clickable& c : menu.clickables()) {
		if (endsWith(c.target, target)) {
			menu.mouseMove(c.x + c.width / 2, c.y + c.height / 2);
			menu.advance(1.0 / 30);
			menu.mouseDown();
			menu.advance(1.0 / 30);
			menu.mouseUp();
			return true;
		}
	}
	return false;
}

bool shown(ui::Player& menu, const std::string& target)
{
	for (const ui::Player::Clickable& c : menu.clickables()) {
		if (endsWith(c.target, target)) {
			return true;
		}
	}
	return false;
}

} // namespace

bool signInForDirectStart(ui::Player& menu, MenuBindings& bindings, const std::string& accountName, double seconds)
{
	if (!menu.loaded() || !menu.interpreter()) {
		return false;
	}
	const std::string accountField = "login.mcAccountName.mcInput.inputField";
	const std::string soldierRow = "login.ScrollTainer.m_mcContainer.contents.holder_list.holder_list.row0";
	const auto mainPage = [&] {
		return bindings.loggedIn() && !bindings.soldierName().empty() && ui::avm1::toBoolean(menu.evaluate("start._visible"));
	};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(seconds * 1000));
	bool accountSent = false;
	int framesSincePopup = -1; // since the last OK, -1 before any
	int soldierFrames = 0;     // the soldier list shown so long (it fills over a few frames)
	int soldierTries = 0;
	while (std::chrono::steady_clock::now() < deadline) {
		menu.advance(1.0 / 30);
		if (mainPage()) {
			return true;
		}
		if (press(menu, "MessageHandler.btnOk")) {
			// "No connection" (OK plays offline), or an error.
			framesSincePopup = 0;
			continue;
		}
		if (framesSincePopup >= 0) {
			++framesSincePopup;
		}
		if (bindings.loggedIn() && shown(menu, soldierRow)) {
			// The first soldier and LOGIN, a second after the list shows
			// (again after three more while the page stays).
			if (++soldierFrames == 30 + 90 * soldierTries) {
				if (soldierTries++ == 3) {
					return false;
				}
				press(menu, soldierRow);
				menu.advance(0.2);
				press(menu, "login.btnLogin");
			}
			continue;
		}
		soldierFrames = 0;
		if (shown(menu, accountField) && shown(menu, "login.btnLogin")) {
			if (accountSent) {
				// Back on the account page a while after a popup: the login
				// failed (an online account wants its password).
				if (framesSincePopup > 60) {
					return false;
				}
			} else {
				const std::string typed = menu.vm().toString(menu.evaluate(accountField + ".text"));
				if (typed.empty()) {
					press(menu, accountField);
					for (const unsigned char c : accountName) {
						menu.keyDown(std::toupper(c), c);
						menu.keyUp(std::toupper(c));
					}
				}
				accountSent = press(menu, "login.btnLogin");
				continue;
			}
		}
		// The online check and the login run on worker threads.
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return mainPage();
}

bool showSingleplayPage(ui::Player& menu)
{
	// The main page comes in over a few seconds; the button answers once
	// it has (pressed each second until the page shows, 8 at most).
	const auto up = [&menu] {
		return menu.evaluate("singleplay.mc_singleplayer").isObject() && ui::avm1::toBoolean(menu.evaluate("singleplay._visible"));
	};
	for (int frame = 1; frame <= 8 * 30 && !up(); ++frame) {
		menu.advance(1.0 / 30);
		if (frame % 30 == 0) {
			press(menu, "FlashMenu.btnSingleplay");
		}
	}
	for (int i = 0; i < 30; ++i) {
		menu.advance(1.0 / 30);
	}
	return up();
}

} // namespace dice::hfe::menu
