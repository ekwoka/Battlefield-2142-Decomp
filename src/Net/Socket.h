// A TCP connection (BSD sockets; Winsock on Windows).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::net {

class TcpSocket {
public:
	TcpSocket();
	~TcpSocket();
	TcpSocket(const TcpSocket&) = delete;
	TcpSocket& operator=(const TcpSocket&) = delete;

	bool connect(const std::string& host, uint16_t port, int timeoutSeconds);
	bool send(const std::vector<uint8_t>& data);
	// Appends what arrives within `timeoutSeconds`: the byte count, 0 on a
	// timeout, -1 once the peer has closed or on errors.
	int receive(std::vector<uint8_t>& out, int timeoutSeconds);
	void close();
	// The native socket (for TlsSocket), -1 when not connected.
	intptr_t native() const { return m_socket; }

private:
	intptr_t m_socket = -1;
};

// A UDP socket talking to one address.
class UdpSocket {
public:
	UdpSocket();
	~UdpSocket();
	UdpSocket(const UdpSocket&) = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	bool open(const std::string& host, uint16_t port);
	bool send(const std::vector<uint8_t>& data);
	// One datagram within `timeoutMilliseconds`: its size, 0 on a timeout,
	// -1 on errors.
	int receive(std::vector<uint8_t>& out, int timeoutMilliseconds);
	void close();

private:
	intptr_t m_socket = -1;
};

// A UDP socket bound to a local port that talks to any address (for
// servers, such as the tests' loopback game server).
class UdpServerSocket {
public:
	struct Address {
		uint32_t ip = 0;   // network order
		uint16_t port = 0; // network order
		bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
	};

	UdpServerSocket();
	~UdpServerSocket();
	UdpServerSocket(const UdpServerSocket&) = delete;
	UdpServerSocket& operator=(const UdpServerSocket&) = delete;

	// Binds 127.0.0.1 (or any address) at `port`; 0 picks a free one.
	bool bind(uint16_t port, bool loopbackOnly = true);
	uint16_t port() const { return m_port; }
	bool sendTo(const Address& to, const std::vector<uint8_t>& data);
	// Lets the socket send to broadcast addresses (SO_BROADCAST).
	bool enableBroadcast();
	// A dotted IPv4 address (or a host name) and a port as an Address.
	static bool resolve(const std::string& host, uint16_t port, Address& out);
	// One datagram within `timeoutMilliseconds`: its size, 0 on a timeout, -1 on errors.
	int receiveFrom(std::vector<uint8_t>& out, Address& from, int timeoutMilliseconds);
	void close();

private:
	intptr_t m_socket = -1;
	uint16_t m_port = 0;
};

} // namespace dice::hfe::net
