// Local stand-ins for OpenSpy's services, on 127.0.0.1 only, built from
// OpenSpy's server code (github.com/openspy/openspy-core: FESL/, GP/,
// cdkey/, serverbrowsing/; github.com/openspy/bf2142-stella_stats for the
// stats web service), so the whole login and join can be tried without a
// live host:
//   - FESL (plain TCP; OpenSpy's is TLS): Hello (with OpenSpy's fields and
//     the MemCheck after it, handle_hello.cpp), Login (101 no such
//     account, 122 wrong password; lkey, displayName, userId, profileId),
//     GetSubAccounts ("subAccounts.N" quoted), LoginSubAccount (lkey,
//     profileId, userId; 101 for another name), GameSpyPreAuth (challenge
//     and ticket, handle_gamespy_preauth.cpp), Goodbye; pings every
//     `pingSeconds`.
//   - GP (gpcm, TCP): \lc\1 with a 10-letter challenge on connecting
//     (GPPeer.cpp), the pre-authenticated login by the ticket
//     (Auth_PreAuth_Token_GPHash: the response checked with the ticket's
//     challenge as the password), \lc\2 with the proof and a 24-digit hex
//     login ticket "lt" (the session key), then empty buddy and block
//     lists; error 260 for a bad response, 265 for an unknown ticket.
//   - the CD-key service (UDP, XOR "gamespy"): \auth\ answered \uok\ as
//     OpenSpy does for any key (cdkey/server/handlers/handle_auth.cpp), or,
//     with `cdKeys` given, only for responses made from one of them
//     (\unok\ otherwise), as GameSpy's own service did.
//   - the master server (TCP, the server browser's list request and the
//     GOA-encrypted reply; ServerBrowser.h) listing `servers`.
//   - the stats service's ValidatePlayer.aspx (HTTP;
//     API/requests/ValidatePlayer.js): "Ok" for the session's soldier,
//     InvalidReportedNick / InvalidReportedProfileID / InvalidAuthProfileID.
// `bf2142 --openspy-emulator` runs it (Main/main.cpp).
#pragma once

#include "Fesl.h"
#include "GameSpyPresence.h"
#include "OnlineConfig.h"
#include "ServerBrowser.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace dice::hfe::net {

struct EmulatorSoldier {
	std::string nick;
	long profileId = 0;
};
struct EmulatorAccount {
	std::string name, password;
	long userId = 0, profileId = 0; // the account's own profile
	std::vector<EmulatorSoldier> soldiers;
};

struct OpenSpyEmulatorOptions {
	// 0 picks free ports (tests); `localEndpoints()`' ports for the CLI.
	uint16_t feslPort = 0, gpcmPort = 0, masterPort = 0, stellaPort = 0, cdkeyPort = 0;
	std::vector<EmulatorAccount> accounts;
	std::vector<std::string> cdKeys; // empty: every key is fine (OpenSpy)
	std::vector<ServerEntry> servers; // what the master lists
	int pingSeconds = 120;            // FESL_PING_TIME
};

class OpenSpyEmulator {
public:
	OpenSpyEmulator();
	~OpenSpyEmulator();
	OpenSpyEmulator(const OpenSpyEmulator&) = delete;
	OpenSpyEmulator& operator=(const OpenSpyEmulator&) = delete;
	bool start(const OpenSpyEmulatorOptions& options, std::string* error = nullptr);
	void stop();
	// Where its services are (FESL plain).
	OnlineEndpoints endpoints() const;
	void addServer(const ServerEntry& server);
	// Each request, as "<service>: <text>".
	std::function<void(const std::string&)> onLog;
	// Counters (tests).
	int gpLogins() const { return m_gpLogins; }
	int validations() const { return m_validations; }

	// A soldier named in "account:password@soldier" form with ids made up.
	static EmulatorAccount account(const std::string& spec, long userId = 1001, long profileId = 2001);

private:
	struct Connection;
	void run();
	void log(const std::string& text);
	void feslMessage(Connection& c, const FeslMessage& m);
	void gpMessage(Connection& c, const GpMessage& m);
	void masterRequest(Connection& c);
	void stellaRequest(Connection& c);
	void cdkeyDatagram();
	const EmulatorSoldier* soldier(long profileId, const EmulatorAccount** owner = nullptr) const;

	OpenSpyEmulatorOptions m_options;
	struct Listeners;
	std::unique_ptr<Listeners> m_listeners;
	std::vector<std::unique_ptr<Connection>> m_connections;
	std::thread m_thread;
	std::atomic<bool> m_running {false};
	mutable std::mutex m_mutex;
	// GameSpyPreAuth's tickets: ticket -> (profile, challenge); GP's
	// sessions: login ticket -> profile.
	std::map<std::string, std::pair<long, std::string>> m_tickets;
	std::map<std::string, long> m_sessions;
	std::atomic<int> m_gpLogins {0}, m_validations {0};
	uint32_t m_nextSession = 1;
};

} // namespace dice::hfe::net
