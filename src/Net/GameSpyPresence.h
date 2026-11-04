// GameSpy Presence (GP, "gpcm"), the GameSpy SDK in BF2142.exe: after the
// account service's GameSpyPreAuth (Fesl.h) the game logs in to GP with
// the ticket it got (gpConnectPreAuthenticated, 0x00717460, called by
// 0x0048b880 with EAOnlineManager +0xa0 the ticket and +0xbc the
// challenge), and keeps GP's login ticket ("lt", gpGetLoginTicket
// 0x00717fb0) as the auth it shows game servers (PlayerAuth.h).
//
// TCP to gpcm.gamespy.com:29900 (0x00729420: the host at 0x00a08b80, port
// 0x74cc; the OpenSpy patch: gpcm.openspy.net). Messages are
// "\key\value..." ending "\final\". The connection:
//   <- \lc\1\challenge\<server challenge>\id\1\final\
//   -> \login\\challenge\<32 random letters and digits>\authtoken\<ticket>
//      \response\<md5>\port\<the peer socket's port, as a signed short>
//      \productid\10709\gamename\stella\namespaceid\20\sdkrevision\3
//      \id\1\final\     (gpiSendLogin 0x00729660; gpInitialize at
//      0x0048b960: product 10709, namespace 0x14)
//   <- \lc\2\sesskey\..\userid\..\profileid\..\uniquenick\..\lt\..
//      \proof\<md5>\id\1\final\   (0x00729c40 checks the proof: "Could
//      not authenticate server." otherwise)
// The response is md5(md5(password) + 48 spaces + user + client challenge
// + server challenge + md5(password)) and the proof the same with the two
// challenges swapped; with an auth token the user is the token and the
// password the partner challenge (0x00729660 takes +0x1c2 over the
// password). Errors are \error\\err\<code>\fatal\\errmsg\<text>\id\1\final\.
// The server's keep-alives (\ka\\final\, every 60 s on OpenSpy) need no
// answer (0x00728a80 only compares them).
#pragma once

#include "Socket.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::net {

constexpr int kGpProductId = 10709;  // *(0x00964238)
constexpr int kGpNamespaceId = 20;   // gpInitialize's 0x14 (OpenSpy's EA partner code)
constexpr uint16_t kGpcmPort = 29900;

// One message's keys and values in order ("\lc\1\challenge\x" -> lc=1, challenge=x).
using GpMessage = std::vector<std::pair<std::string, std::string>>;
std::string gpValue(const GpMessage& message, const std::string& key);
bool gpHas(const GpMessage& message, const std::string& key);
// The complete messages at the front of `buffer` (up to each "\final\").
std::vector<GpMessage> takeGpMessages(std::string& buffer);
std::string encodeGp(const GpMessage& message); // with "\final\"

std::string gpLoginResponse(const std::string& password, const std::string& user, const std::string& clientChallenge,
	const std::string& serverChallenge);
std::string gpLoginProof(const std::string& password, const std::string& user, const std::string& clientChallenge,
	const std::string& serverChallenge);

struct GpSession {
	long sessionKey = 0, userId = 0, profileId = 0;
	std::string uniquenick;
	std::string loginTicket; // "lt": the auth a game server is shown
};

class GpClient {
public:
	bool connect(const std::string& host, uint16_t port, int timeoutSeconds = 10, std::string* error = nullptr);
	// The pre-authenticated login (gpConnectPreAuthenticated) with
	// GameSpyPreAuth's ticket and challenge.
	std::optional<GpSession> loginPreAuthenticated(const std::string& authToken, const std::string& partnerChallenge,
		std::string* error = nullptr, int timeoutMilliseconds = 10000);
	// Reads what has come (keep-alives, buddy lists); false once closed.
	bool pump(int timeoutMilliseconds = 0);
	void logout();
	bool connected() const { return m_socket.connected(); }
	// The login exactly as sent (tests).
	const std::string& lastLogin() const { return m_lastLogin; }

private:
	std::optional<GpMessage> waitFor(const std::string& key, int timeoutMilliseconds);
	TcpSocket m_socket;
	TcpListener m_peer; // the SDK's peer socket (gpiConnect 0x00729420 binds and listens on any port)
	std::string m_buffer;
	std::string m_lastLogin;
	long m_sessionKey = 0;
};

} // namespace dice::hfe::net
