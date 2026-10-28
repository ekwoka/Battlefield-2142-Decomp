// The client side of the game server's remote console (admin/default.py,
// the TCP "RCON/admin" module the dedicated server loads as
// sv.adminScript "default"):
//
//   - the server greets with lines starting "### " and ends the greeting
//     with an empty line; one of them is "### Digest seed: <16 letters>";
//   - "login <md5 hex of seed + password>" authenticates (the password
//     is the one in <overlay>admin/default.cfg);
//   - "exec <console command>" runs a console command on the server
//     (host.rcon_invoke) and answers with its output; "users" lists the
//     authenticated users;
//   - a command starting with byte 0x02 gets its reply terminated by
//     byte 0x04 (the "non-interactive" form this client uses).
//
// Commands are lines ending with '\n'; a line over 128 bytes without one
// closes the connection.
#pragma once

#include "Socket.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::net {

class RemoteConsoleClient {
public:
	// Connects and reads the greeting (false on no connection or no seed).
	bool connect(const std::string& host, uint16_t port, int timeoutSeconds = 5, std::string* error = nullptr);
	const std::string& seed() const { return m_seed; }
	const std::string& greeting() const { return m_greeting; }
	// "login" with the seeded digest; true when the server says
	// "Authentication successful".
	bool login(const std::string& password, std::string* reply = nullptr);
	// "exec <command>": the console's output.
	bool exec(const std::string& command, std::string& reply);
	// Any line (without the 0x02, which this adds); the reply up to 0x04.
	bool request(const std::string& line, std::string& reply);
	void close() { m_socket.close(); }

	// The digest default.py's digest() makes: md5 hex of seed + password.
	static std::string digest(const std::string& seed, const std::string& password);

private:
	bool readUntil(const std::string& terminator, std::string& out);

	TcpSocket m_socket;
	std::string m_seed;
	std::string m_greeting;
	std::string m_pending; // received after the last reply
	int m_timeout = 5;
};

} // namespace dice::hfe::net
