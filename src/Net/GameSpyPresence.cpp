#include "GameSpyPresence.h"

#include "Md5.h"
#include "OnlineConfig.h"

#include <chrono>
#include <cstdlib>
#include <random>

namespace dice::hfe::net {

namespace {

const std::string kSpaces(48, ' ');

} // namespace

std::string gpValue(const GpMessage& message, const std::string& key)
{
	for (const auto& [k, v] : message) {
		if (k == key) {
			return v;
		}
	}
	return {};
}

bool gpHas(const GpMessage& message, const std::string& key)
{
	for (const auto& [k, v] : message) {
		if (k == key) {
			return true;
		}
	}
	return false;
}

std::vector<GpMessage> takeGpMessages(std::string& buffer)
{
	std::vector<GpMessage> out;
	for (size_t end; (end = buffer.find("\\final\\")) != std::string::npos;) {
		const std::string text = buffer.substr(0, end);
		buffer.erase(0, end + 7);
		// "\k\v\k\v": split on backslashes after the first.
		GpMessage message;
		std::vector<std::string> parts;
		size_t at = text.empty() || text[0] != '\\' ? 0 : 1;
		while (at <= text.size()) {
			const size_t next = text.find('\\', at);
			parts.push_back(text.substr(at, next == std::string::npos ? std::string::npos : next - at));
			if (next == std::string::npos) {
				break;
			}
			at = next + 1;
		}
		for (size_t i = 0; i + 1 < parts.size() || i < parts.size(); i += 2) {
			message.emplace_back(parts[i], i + 1 < parts.size() ? parts[i + 1] : std::string());
		}
		out.push_back(std::move(message));
	}
	return out;
}

std::string encodeGp(const GpMessage& message)
{
	std::string out;
	for (const auto& [k, v] : message) {
		out += "\\" + k + "\\" + v;
	}
	return out + "\\final\\";
}

std::string gpLoginResponse(const std::string& password, const std::string& user, const std::string& clientChallenge,
	const std::string& serverChallenge)
{
	// 0x00729660: sprintf("%s%s%s%s%s%s", md5(pw), 48 spaces, user,
	// client challenge (+0x80), server challenge (+0), md5(pw)).
	const std::string pw = md5Hex(password);
	return md5Hex(pw + kSpaces + user + clientChallenge + serverChallenge + pw);
}

std::string gpLoginProof(const std::string& password, const std::string& user, const std::string& clientChallenge,
	const std::string& serverChallenge)
{
	// 0x00729c40: the server's proof, the challenges the other way round.
	const std::string pw = md5Hex(password);
	return md5Hex(pw + kSpaces + user + serverChallenge + clientChallenge + pw);
}

bool GpClient::connect(const std::string& host, uint16_t port, int timeoutSeconds, std::string* error)
{
	if (!hostAllowed(host)) {
		if (error) {
			*error = hostRefusal(host);
		}
		return false;
	}
	m_buffer.clear();
	// The peer socket first (0x00729420 binds it before the connection).
	m_peer.listen(0, !onlineConfig().online);
	if (!m_socket.connect(host, port, timeoutSeconds)) {
		if (error) {
			*error = "cannot connect to " + host + ":" + std::to_string(port);
		}
		return false;
	}
	return true;
}

std::optional<GpMessage> GpClient::waitFor(const std::string& key, int timeoutMilliseconds)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
	for (;;) {
		std::vector<uint8_t> data;
		for (GpMessage& m : takeGpMessages(m_buffer)) {
			if (!m.empty() && (m.front().first == key || m.front().first == "error")) {
				return m;
			}
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return std::nullopt;
		}
		const int got = m_socket.receiveFor(data, 100);
		if (got < 0) {
			m_socket.close();
			return std::nullopt;
		}
		m_buffer.append(data.begin(), data.end());
	}
}

std::optional<GpSession> GpClient::loginPreAuthenticated(const std::string& authToken, const std::string& partnerChallenge,
	std::string* error, int timeoutMilliseconds)
{
	const auto fail = [error](const std::string& why) -> std::optional<GpSession> {
		if (error) {
			*error = why;
		}
		return std::nullopt;
	};
	auto challenge = waitFor("lc", timeoutMilliseconds);
	if (!challenge || gpValue(*challenge, "lc") != "1") {
		return fail(challenge && challenge->front().first == "error" ? "GP error " + gpValue(*challenge, "err") + ": "
					+ gpValue(*challenge, "errmsg")
																	   : "no login challenge");
	}
	const std::string serverChallenge = gpValue(*challenge, "challenge");
	// 0x00729660's client challenge: 32 of the 62 letters and digits by rand().
	static const char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	static std::mt19937 random {std::random_device {}()};
	std::string clientChallenge;
	for (int i = 0; i < 32; ++i) {
		clientChallenge += kLetters[random() % 62];
	}
	const std::string response = gpLoginResponse(partnerChallenge, authToken, clientChallenge, serverChallenge);
	// htons of the peer socket's port, pushed through a movsx: a signed short.
	const int16_t port = static_cast<int16_t>(m_peer.port());
	GpMessage login {{"login", ""}, {"challenge", clientChallenge}, {"authtoken", authToken}, {"response", response},
		{"port", std::to_string(port)}, {"productid", std::to_string(kGpProductId)}, {"gamename", "stella"},
		{"namespaceid", std::to_string(kGpNamespaceId)}, {"sdkrevision", "3"}, {"id", "1"}};
	m_lastLogin = encodeGp(login);
	if (!m_socket.send(std::vector<uint8_t>(m_lastLogin.begin(), m_lastLogin.end()))) {
		return fail("cannot send the login");
	}
	auto reply = waitFor("lc", timeoutMilliseconds);
	if (!reply) {
		return fail("no answer to the login");
	}
	if (reply->front().first == "error") {
		return fail("GP error " + gpValue(*reply, "err") + ": " + gpValue(*reply, "errmsg"));
	}
	if (gpValue(*reply, "lc") != "2") {
		return fail("unexpected data from the server");
	}
	if (gpValue(*reply, "proof") != gpLoginProof(partnerChallenge, authToken, clientChallenge, serverChallenge)) {
		return fail("Could not authenticate server."); // 0x00729c40, error 0x108
	}
	GpSession session;
	session.sessionKey = std::atol(gpValue(*reply, "sesskey").c_str());
	session.userId = std::atol(gpValue(*reply, "userid").c_str());
	session.profileId = std::atol(gpValue(*reply, "profileid").c_str());
	session.uniquenick = gpValue(*reply, "uniquenick").substr(0, 20);
	session.loginTicket = gpValue(*reply, "lt").substr(0, 24); // +0x488, 0x19 bytes
	m_sessionKey = session.sessionKey;
	return session;
}

bool GpClient::pump(int timeoutMilliseconds)
{
	if (!m_socket.connected()) {
		return false;
	}
	std::vector<uint8_t> data;
	if (m_socket.receiveFor(data, timeoutMilliseconds) < 0) {
		m_socket.close();
		return false;
	}
	m_buffer.append(data.begin(), data.end());
	takeGpMessages(m_buffer); // keep-alives and buddy lists: nothing to do
	return true;
}

void GpClient::logout()
{
	if (m_socket.connected()) {
		const std::string bye = "\\logout\\\\sesskey\\" + std::to_string(m_sessionKey) + "\\final\\";
		m_socket.send(std::vector<uint8_t>(bye.begin(), bye.end()));
	}
	m_socket.close();
	m_peer.close();
}

} // namespace dice::hfe::net
