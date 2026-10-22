// EA's account service ("FESL", feslsdk 2.8.0.0 in BF2142.exe): the game
// logs in here before it browses or joins, to get the player's soldier
// (a "sub-account", a GameSpy profile) whose profile id it shows game
// servers. The game connects to bf2142-pc.fesl.ea.com:18300 (0x004a0000's
// call of 0x00800c60); the OpenSpy patch (RendDX9.dll 0x1004aab0) sends it
// to fesl.openspy.net:18301 over TLS instead. OpenSpy's server is
// openspy-core/code/FESL.
//
// Every message: a 12-byte big-endian header (a four-letter service,
// "fsys" for the connection or "acct" for accounts; an id word; the whole
// length) and "key=value\n" lines ending with a NUL. TXN names the
// transaction; TID numbers it and the reply repeats it. Values may be
// quoted and URL-encoded ("curTime=\"Oct-21-2026%2012%3a00%3a00%20UTC\"").
// Lists are "name.[]=count" then "name.0=...".
//
// The conversation (0x008154f0 Hello, 0x00801ef0 Login):
//   fsys Hello (clientString, sku, locale, clientPlatform, clientVersion,
//     SDKVersion, protocolVersion 2.0, fragmentSize 2048)
//   <- fsys Hello (theater and messenger hosts, curTime), fsys MemCheck
//   fsys MemCheck (result: the salt mixed with memory checks, none asked)
//   acct Login (name, password, machineId, macAddr) <- lkey, userId, profileId
//   acct GetSubAccounts <- subAccounts.N (soldier names)
//   acct LoginSubAccount (name) <- profileId of that soldier
//   acct GameSpyPreAuth <- ticket (for GameSpy's presence and stats)
// The server sends fsys Ping now and then; the client answers it.
#pragma once

#include "TlsSocket.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::net {

constexpr const char* kFeslHost = "fesl.openspy.net";
constexpr uint16_t kFeslPort = 18301;      // OpenSpy's TLS port
constexpr uint16_t kFeslGamePort = 18300;  // what the unpatched game uses
constexpr uint32_t kFeslSystem = 0x66737973;  // "fsys"
constexpr uint32_t kFeslAccount = 0x61636374; // "acct"

struct FeslMessage {
	uint32_t type = 0;
	uint32_t id = 0;
	std::vector<std::pair<std::string, std::string>> fields;

	// The raw value (empty when missing).
	const std::string* find(const std::string& key) const;
	std::string get(const std::string& key) const;
	// The value unquoted and URL-decoded.
	std::string text(const std::string& key) const;
	long number(const std::string& key, long otherwise = 0) const;
	std::string txn() const { return get("TXN"); }
	bool isError() const { return find("errorCode") != nullptr; }
	// A list's values: name.0, name.1, ... (count from name.[]).
	std::vector<std::string> list(const std::string& name) const;
};

std::vector<uint8_t> encodeFesl(const FeslMessage& message);
// Takes the complete messages off the front of `buffer`. Sets `bad` when
// a header cannot be right (the stream cannot be followed after it).
std::vector<FeslMessage> takeFeslMessages(std::vector<uint8_t>& buffer, bool* bad = nullptr);
std::string feslUnquote(const std::string& value);
// errorCode values (OpenSpy's FESL_ERROR).
const char* feslErrorName(long code);

struct FeslAccount {
	std::string displayName;
	std::string lkey; // the session key
	long userId = 0;
	long profileId = 0;   // the account's own profile
	std::vector<std::string> soldiers;
};

class FeslClient {
public:
	bool connect(const std::string& host = kFeslHost, uint16_t port = kFeslPort, int timeoutSeconds = 10,
		std::string* error = nullptr);
	// Each sends one request and waits for its reply (answering pings and
	// memory checks meanwhile); empty on a timeout or a closed connection.
	std::optional<FeslMessage> hello(int timeoutMilliseconds = 10000);
	std::optional<FeslMessage> login(const std::string& name, const std::string& password, int timeoutMilliseconds = 10000);
	std::optional<FeslMessage> subAccounts(int timeoutMilliseconds = 10000);
	std::optional<FeslMessage> loginSubAccount(const std::string& soldier, int timeoutMilliseconds = 10000);
	std::optional<FeslMessage> gameSpyPreAuth(int timeoutMilliseconds = 10000);
	void goodbye();

	std::optional<FeslMessage> request(uint32_t type, std::vector<std::pair<std::string, std::string>> fields,
		int timeoutMilliseconds);
	bool connected() const { return m_connected; }

private:
	bool send(const FeslMessage& message);
	void answer(const FeslMessage& message);

	TlsSocket m_socket;
	std::vector<uint8_t> m_received;
	uint32_t m_nextTid = 1;
	bool m_connected = false;
};

// Logs in and picks a soldier (the first when `soldier` is empty): the
// whole sequence the game's login screen runs. `error` says what failed.
struct FeslSession {
	FeslAccount account;
	std::string soldier;
	long soldierProfileId = 0;
	std::string ticket;
};
std::optional<FeslSession> feslLogin(FeslClient& client, const std::string& name, const std::string& password,
	const std::string& soldier, std::string* error = nullptr);

} // namespace dice::hfe::net
