// The whole login a retail client does before it joins a server, against
// the configured services (OnlineConfig.h):
//   1. FESL (Fesl.h): Hello, Login, GetSubAccounts (the menu's account
//      page, EAOnlineManager 0x004a1d70 cases 1-3);
//   2. FESL LoginSubAccount (the soldier: its profile id, and the name's
//      checksum, 0x004a1d70 case 0xb) and GameSpyPreAuth (case 0x13: the
//      ticket and challenge);
//   3. GameSpy Presence (GameSpyPresence.h): the pre-authenticated login
//      with that ticket (0x0048b880), whose login ticket is the auth the
//      game server is shown (0x0048b350 -> EAOnlineManager slot 0x70).
// Then both connections stay open (keepAlive: a thread answering FESL's
// pings and reading GP's keep-alives) while the game runs, as the retail
// client keeps them; logout() says goodbye.
#pragma once

#include "Fesl.h"
#include "GameSpyPresence.h"
#include "OnlineConfig.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace dice::hfe::net {

// What a game connection needs from the login (GameClientOptions).
struct OnlineIdentity {
	std::string soldier;
	long profileId = 0;
	uint32_t nameChecksum = 0;
	std::string loginTicket; // GP's "lt"
	std::string cdKey;
	bool valid() const { return profileId > 0; }
};

class OnlineSession {
public:
	~OnlineSession();
	// Step 1. `error` says what failed; `feslError` the errorCode (0 when
	// the service wasn't reached or answered oddly).
	std::optional<FeslAccount> loginAccount(const std::string& name, const std::string& password, std::string* error = nullptr,
		long* feslError = nullptr);
	// Steps 2 and 3 (the first soldier when `soldier` is empty).
	bool loginSoldier(const std::string& soldier, std::string* error = nullptr, long* feslError = nullptr);
	// All three with the configured credentials (or the ones given).
	bool login(const std::string& name, const std::string& password, const std::string& soldier, std::string* error = nullptr);
	void keepAlive();
	void logout();
	// Services other than the configured ones (tests' emulators).
	void setEndpoints(const OnlineEndpoints& endpoints) { m_endpoints = endpoints; }
	void setCdKey(const std::string& key) { m_cdKey = key; }

	const FeslAccount& account() const { return m_account; }
	OnlineIdentity identity() const;
	bool loggedIn() const { return m_identity.valid(); }
	// Progress lines ("fesl: logged in as ...").
	std::function<void(const std::string&)> onLog;

private:
	void log(const std::string& text) const;
	mutable std::mutex m_mutex;
	FeslClient m_fesl;
	GpClient m_gp;
	FeslAccount m_account;
	OnlineIdentity m_identity;
	std::optional<OnlineEndpoints> m_endpoints;
	std::optional<std::string> m_cdKey;
	std::thread m_keepAlive;
	std::atomic<bool> m_running {false};
};

} // namespace dice::hfe::net
