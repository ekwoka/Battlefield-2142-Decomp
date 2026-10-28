#include "RemoteConsole.h"

#include "Md5.h"

#include <chrono>

namespace dice::hfe::net {

std::string RemoteConsoleClient::digest(const std::string& seed, const std::string& password)
{
	return md5Hex(seed + password);
}

bool RemoteConsoleClient::readUntil(const std::string& terminator, std::string& out)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(m_timeout);
	for (;;) {
		const size_t at = m_pending.find(terminator);
		if (at != std::string::npos) {
			out = m_pending.substr(0, at);
			m_pending.erase(0, at + terminator.size());
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::vector<uint8_t> data;
		const int n = m_socket.receive(data, 1);
		if (n < 0) {
			return false;
		}
		m_pending.append(data.begin(), data.end());
	}
}

bool RemoteConsoleClient::connect(const std::string& host, uint16_t port, int timeoutSeconds, std::string* error)
{
	m_timeout = timeoutSeconds;
	m_pending.clear();
	m_seed.clear();
	if (!m_socket.connect(host, port, timeoutSeconds)) {
		if (error) {
			*error = "cannot connect to " + host + ":" + std::to_string(port);
		}
		return false;
	}
	// The welcome message ends with "\n\n".
	if (!readUntil("\n\n", m_greeting)) {
		if (error) {
			*error = "no greeting";
		}
		return false;
	}
	const std::string marker = "### Digest seed: ";
	const size_t at = m_greeting.find(marker);
	if (at != std::string::npos) {
		const size_t start = at + marker.size();
		m_seed = m_greeting.substr(start, m_greeting.find('\n', start) - start);
	}
	if (m_seed.empty() && error) {
		*error = "no digest seed in the greeting";
	}
	return !m_seed.empty();
}

bool RemoteConsoleClient::request(const std::string& line, std::string& reply)
{
	const std::string text = "\x02" + line + "\n";
	if (!m_socket.send(std::vector<uint8_t>(text.begin(), text.end()))) {
		return false;
	}
	return readUntil("\x04", reply);
}

bool RemoteConsoleClient::login(const std::string& password, std::string* reply)
{
	std::string answer;
	if (!request("login " + digest(m_seed, password), answer)) {
		return false;
	}
	if (reply) {
		*reply = answer;
	}
	return answer.find("Authentication successful") != std::string::npos;
}

bool RemoteConsoleClient::exec(const std::string& command, std::string& reply)
{
	return request("exec " + command, reply);
}

} // namespace dice::hfe::net
