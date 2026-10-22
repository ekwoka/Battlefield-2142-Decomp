#include "Socket.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>

namespace dice::hfe::net {

namespace {

#ifdef _WIN32
struct WinsockStartup {
	WinsockStartup()
	{
		WSADATA data;
		WSAStartup(MAKEWORD(2, 2), &data);
	}
	~WinsockStartup() { WSACleanup(); }
};
void startup()
{
	static WinsockStartup started;
}
void closeSocket(intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
using Native = SOCKET;
#else
using Native = int;
void startup() {}
void closeSocket(intptr_t s) { ::close(static_cast<int>(s)); }
#endif

bool waitFor(intptr_t s, bool write, int seconds)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(static_cast<Native>(s), &set);
	timeval timeout {seconds, 0};
	const int ready = select(static_cast<int>(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout);
	return ready > 0;
}

} // namespace

TcpSocket::TcpSocket() { startup(); }
TcpSocket::~TcpSocket() { close(); }

void TcpSocket::close()
{
	if (m_socket != -1) {
		closeSocket(m_socket);
		m_socket = -1;
	}
}

bool TcpSocket::connect(const std::string& host, uint16_t port, int)
{
	addrinfo hints {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* found = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
		return false;
	}
	bool connected = false;
	for (addrinfo* a = found; a && !connected; a = a->ai_next) {
		const auto s = static_cast<intptr_t>(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
		if (s == -1) {
			continue;
		}
		if (::connect(static_cast<Native>(s), a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
			m_socket = s;
			connected = true;
		} else {
			closeSocket(s);
		}
	}
	freeaddrinfo(found);
	return connected;
}

bool TcpSocket::send(const std::vector<uint8_t>& data)
{
	size_t sent = 0;
	while (sent < data.size()) {
		const auto n = ::send(static_cast<Native>(m_socket),
			reinterpret_cast<const char*>(data.data() + sent), static_cast<int>(data.size() - sent), 0);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

int TcpSocket::receive(std::vector<uint8_t>& out, int timeoutSeconds)
{
	if (m_socket == -1) {
		return -1;
	}
	if (!waitFor(m_socket, false, timeoutSeconds)) {
		return 0;
	}
	char buffer[8192];
	const auto n = ::recv(static_cast<Native>(m_socket), buffer, sizeof(buffer), 0);
	if (n <= 0) {
		return -1;
	}
	out.insert(out.end(), buffer, buffer + n);
	return static_cast<int>(n);
}

UdpSocket::UdpSocket() { startup(); }
UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close()
{
	if (m_socket != -1) {
		closeSocket(m_socket);
		m_socket = -1;
	}
}

// A connected UDP socket: send and receive talk to that address only.
bool UdpSocket::open(const std::string& host, uint16_t port)
{
	addrinfo hints {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* found = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
		return false;
	}
	const auto s = static_cast<intptr_t>(::socket(found->ai_family, found->ai_socktype, found->ai_protocol));
	bool ok = s != -1 && ::connect(static_cast<Native>(s), found->ai_addr, static_cast<int>(found->ai_addrlen)) == 0;
	freeaddrinfo(found);
	if (!ok) {
		if (s != -1) {
			closeSocket(s);
		}
		return false;
	}
	m_socket = s;
	return true;
}

bool UdpSocket::send(const std::vector<uint8_t>& data)
{
	return m_socket != -1
		&& ::send(static_cast<Native>(m_socket), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0)
		== static_cast<int>(data.size());
}

int UdpSocket::receive(std::vector<uint8_t>& out, int timeoutMilliseconds)
{
	if (m_socket == -1) {
		return -1;
	}
	fd_set set;
	FD_ZERO(&set);
	FD_SET(static_cast<Native>(m_socket), &set);
	timeval timeout {timeoutMilliseconds / 1000, (timeoutMilliseconds % 1000) * 1000};
	if (select(static_cast<int>(m_socket) + 1, &set, nullptr, nullptr, &timeout) <= 0) {
		return 0;
	}
	char buffer[2048];
	const auto n = ::recv(static_cast<Native>(m_socket), buffer, sizeof(buffer), 0);
	if (n < 0) {
		return -1;
	}
	out.assign(buffer, buffer + n);
	return static_cast<int>(n);
}

UdpServerSocket::UdpServerSocket() { startup(); }
UdpServerSocket::~UdpServerSocket() { close(); }

void UdpServerSocket::close()
{
	if (m_socket != -1) {
		closeSocket(m_socket);
		m_socket = -1;
	}
}

bool UdpServerSocket::bind(uint16_t port, bool loopbackOnly)
{
	close();
	const auto s = static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
	if (s == -1) {
		return false;
	}
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
	if (::bind(static_cast<Native>(s), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
		closeSocket(s);
		return false;
	}
	socklen_t length = sizeof(address);
	getsockname(static_cast<Native>(s), reinterpret_cast<sockaddr*>(&address), &length);
	m_port = ntohs(address.sin_port);
	m_socket = s;
	return true;
}

bool UdpServerSocket::sendTo(const Address& to, const std::vector<uint8_t>& data)
{
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_port = to.port;
	address.sin_addr.s_addr = to.ip;
	return m_socket != -1
		&& ::sendto(static_cast<Native>(m_socket), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
			   0, reinterpret_cast<sockaddr*>(&address), sizeof(address))
		== static_cast<int>(data.size());
}

int UdpServerSocket::receiveFrom(std::vector<uint8_t>& out, Address& from, int timeoutMilliseconds)
{
	if (m_socket == -1) {
		return -1;
	}
	fd_set set;
	FD_ZERO(&set);
	FD_SET(static_cast<Native>(m_socket), &set);
	timeval timeout {timeoutMilliseconds / 1000, (timeoutMilliseconds % 1000) * 1000};
	if (select(static_cast<int>(m_socket) + 1, &set, nullptr, nullptr, &timeout) <= 0) {
		return 0;
	}
	char buffer[2048];
	sockaddr_in address {};
	socklen_t length = sizeof(address);
	const auto n = ::recvfrom(static_cast<Native>(m_socket), buffer, sizeof(buffer), 0,
		reinterpret_cast<sockaddr*>(&address), &length);
	if (n < 0) {
		return -1;
	}
	from.ip = address.sin_addr.s_addr;
	from.port = address.sin_port;
	out.assign(buffer, buffer + n);
	return static_cast<int>(n);
}

} // namespace dice::hfe::net
