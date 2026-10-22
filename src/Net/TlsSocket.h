// A TLS connection over TCP, for the EA account service (FESL). The game
// links its own SSL code (fesl.ea.com's certificate, SSLv3); the OpenSpy
// patch (RendDX9.dll 0x1004aab0) replaces it with OpenSSL and a modern
// connection to fesl.openspy.net, so this uses OpenSSL too, with the host
// name sent for SNI and the certificate verified.
//
// Built only when CMake finds OpenSSL (BF2142_HAVE_TLS); without it
// connect() fails.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::net {

class TlsSocket {
public:
	TlsSocket();
	~TlsSocket();
	TlsSocket(const TlsSocket&) = delete;
	TlsSocket& operator=(const TlsSocket&) = delete;

	// Connects and completes the handshake; `error` says why not.
	bool connect(const std::string& host, uint16_t port, int timeoutSeconds, std::string* error = nullptr);
	bool send(const std::vector<uint8_t>& data);
	// Appends what arrives within `timeoutMilliseconds`: the byte count, 0
	// on a timeout, -1 once the peer has closed or on errors.
	int receive(std::vector<uint8_t>& out, int timeoutMilliseconds);
	void close();

	static bool available();

private:
	struct State;
	std::unique_ptr<State> m_state;
};

} // namespace dice::hfe::net
