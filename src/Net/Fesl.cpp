#include "Fesl.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace dice::hfe::net {

namespace {

constexpr size_t kHeaderSize = 12;
// The client's id word: 0xC0000000 with the transaction number (servers
// answer with 0x80000000 and their own count; OpenSpy reads only TID).
constexpr uint32_t kClientRequest = 0xc0000000;

void put32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int shift = 24; shift >= 0; shift -= 8) {
		out.push_back(static_cast<uint8_t>(v >> shift));
	}
}

uint32_t get32(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

int hexDigit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

} // namespace

const char* feslErrorName(long code)
{
	switch (code) {
	case 21: return "invalid field";
	case 99: return "system error";
	case 101: return "no such account";
	case 122: return "wrong password";
	case 160: return "the account exists";
	default: return "error";
	}
}

const std::string* FeslMessage::find(const std::string& key) const
{
	for (const auto& [name, value] : fields) {
		if (name == key) {
			return &value;
		}
	}
	return nullptr;
}

std::string FeslMessage::get(const std::string& key) const
{
	const std::string* value = find(key);
	return value ? *value : std::string();
}

std::string FeslMessage::text(const std::string& key) const { return feslUnquote(get(key)); }

long FeslMessage::number(const std::string& key, long otherwise) const
{
	const std::string* value = find(key);
	return value && !value->empty() ? std::strtol(value->c_str(), nullptr, 10) : otherwise;
}

std::vector<std::string> FeslMessage::list(const std::string& name) const
{
	std::vector<std::string> values;
	const long count = number(name + ".[]");
	for (long i = 0; i < count; ++i) {
		values.push_back(text(name + "." + std::to_string(i)));
	}
	return values;
}

std::string feslUnquote(const std::string& value)
{
	std::string s = value;
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
		s = s.substr(1, s.size() - 2);
	}
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size() && hexDigit(s[i + 1]) >= 0 && hexDigit(s[i + 2]) >= 0) {
			out += static_cast<char>(hexDigit(s[i + 1]) * 16 + hexDigit(s[i + 2]));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::vector<uint8_t> encodeFesl(const FeslMessage& message)
{
	std::string body;
	for (const auto& [key, value] : message.fields) {
		body += key + "=" + value + "\n";
	}
	std::vector<uint8_t> out;
	put32(out, message.type);
	put32(out, message.id);
	put32(out, static_cast<uint32_t>(kHeaderSize + body.size() + 1));
	out.insert(out.end(), body.begin(), body.end());
	out.push_back(0);
	return out;
}

std::vector<FeslMessage> takeFeslMessages(std::vector<uint8_t>& buffer, bool* bad)
{
	std::vector<FeslMessage> messages;
	size_t at = 0;
	while (buffer.size() - at >= kHeaderSize) {
		const uint32_t length = get32(buffer.data() + at + 8);
		if (length < kHeaderSize || length > (1u << 20)) {
			if (bad) {
				*bad = true;
			}
			buffer.clear();
			return messages;
		}
		if (buffer.size() - at < length) {
			break;
		}
		FeslMessage message;
		message.type = get32(buffer.data() + at);
		message.id = get32(buffer.data() + at + 4);
		std::string body(reinterpret_cast<const char*>(buffer.data() + at + kHeaderSize), length - kHeaderSize);
		if (const size_t nul = body.find('\0'); nul != std::string::npos) {
			body.resize(nul);
		}
		size_t line = 0;
		while (line < body.size()) {
			size_t end = body.find('\n', line);
			if (end == std::string::npos) {
				end = body.size();
			}
			const std::string text = body.substr(line, end - line);
			const size_t equals = text.find('=');
			if (equals != std::string::npos) {
				message.fields.emplace_back(text.substr(0, equals), text.substr(equals + 1));
			}
			line = end + 1;
		}
		messages.push_back(std::move(message));
		at += length;
	}
	buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(at));
	return messages;
}

bool FeslClient::connect(const std::string& host, uint16_t port, int timeoutSeconds, std::string* error)
{
	m_connected = m_socket.connect(host, port, timeoutSeconds, error);
	m_received.clear();
	m_nextTid = 1;
	return m_connected;
}

bool FeslClient::send(const FeslMessage& message)
{
	if (!m_connected || !m_socket.send(encodeFesl(message))) {
		m_connected = false;
		return false;
	}
	return true;
}

// The server's own requests: keep-alive pings and memory checks.
void FeslClient::answer(const FeslMessage& message)
{
	if (message.type != kFeslSystem) {
		return;
	}
	const std::string txn = message.txn();
	if (txn == "Ping") {
		send({kFeslSystem, 0x80000000, {{"TXN", "Ping"}}});
	} else if (txn == "MemCheck") {
		// 0x008151c0: the result is the checked memory's hash XOR the salt;
		// with no regions asked (memcheck.[]=0) it stays empty.
		send({kFeslSystem, 0x80000000, {{"TXN", "MemCheck"}, {"result", ""}}});
	}
}

std::optional<FeslMessage> FeslClient::request(uint32_t type, std::vector<std::pair<std::string, std::string>> fields,
	int timeoutMilliseconds)
{
	const uint32_t tid = m_nextTid++;
	FeslMessage message {type, kClientRequest | tid, {}};
	message.fields.emplace_back("TXN", fields.empty() ? std::string() : fields.front().second);
	message.fields.emplace_back("TID", std::to_string(tid));
	message.fields.insert(message.fields.end(), fields.begin() + (fields.empty() ? 0 : 1), fields.end());
	const std::string txn = message.fields.front().second;
	if (!send(message)) {
		return std::nullopt;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
	std::vector<FeslMessage> waiting;
	while (std::chrono::steady_clock::now() < deadline) {
		const int received = m_socket.receive(m_received, 100);
		if (received < 0) {
			m_connected = false;
			return std::nullopt;
		}
		bool bad = false;
		for (FeslMessage& reply : takeFeslMessages(m_received, &bad)) {
			if (reply.type == type && reply.txn() == txn && (reply.find("TID") == nullptr || reply.number("TID") == tid)) {
				return reply;
			}
			answer(reply);
		}
		if (bad) {
			m_connected = false;
			return std::nullopt;
		}
	}
	return std::nullopt;
}

std::optional<FeslMessage> FeslClient::hello(int timeoutMilliseconds)
{
	// 0x008154f0, with what 0x004a0000 passes: the client string, the
	// language's SKU and locale, and the SDK's own version.
	return request(kFeslSystem,
		{{"TXN", "Hello"}, {"clientString", "bf2142-pc"}, {"sku", "bf2142-pc"}, {"locale", "en_US"},
			{"clientPlatform", "PC"}, {"clientVersion", "1.51"}, {"SDKVersion", "2.8.0.0"}, {"protocolVersion", "2.0"},
			{"fragmentSize", "2048"}},
		timeoutMilliseconds);
}

std::optional<FeslMessage> FeslClient::login(const std::string& name, const std::string& password, int timeoutMilliseconds)
{
	// 0x00801ef0 (no saved encrypted login).
	return request(kFeslAccount,
		{{"TXN", "Login"}, {"returnEncryptedInfo", "0"}, {"name", name}, {"password", password}, {"machineId", ""},
			{"macAddr", "$000000000000"}},
		timeoutMilliseconds);
}

std::optional<FeslMessage> FeslClient::subAccounts(int timeoutMilliseconds)
{
	return request(kFeslAccount, {{"TXN", "GetSubAccounts"}}, timeoutMilliseconds);
}

std::optional<FeslMessage> FeslClient::loginSubAccount(const std::string& soldier, int timeoutMilliseconds)
{
	return request(kFeslAccount, {{"TXN", "LoginSubAccount"}, {"name", soldier}}, timeoutMilliseconds);
}

std::optional<FeslMessage> FeslClient::gameSpyPreAuth(int timeoutMilliseconds)
{
	return request(kFeslAccount, {{"TXN", "GameSpyPreAuth"}}, timeoutMilliseconds);
}

void FeslClient::goodbye()
{
	if (m_connected) {
		send({kFeslSystem, kClientRequest | m_nextTid++, {{"TXN", "Goodbye"}, {"reason", "GOODBYE_CLIENT_NORMAL"}}});
	}
	m_socket.close();
	m_connected = false;
}

std::optional<FeslSession> feslLogin(FeslClient& client, const std::string& name, const std::string& password,
	const std::string& soldier, std::string* error)
{
	const auto fail = [error](const std::string& step, const std::optional<FeslMessage>& reply) {
		if (error) {
			*error = step + (reply ? std::string(": ") + feslErrorName(reply->number("errorCode")) + " (" + reply->get("errorCode") + ")" +
										   (reply->find("errorContainer.0.fieldError") ? ": " + reply->text("errorContainer.0.fieldError") : "")
									 : ": no reply");
		}
		return std::nullopt;
	};
	if (!client.connected()) {
		std::string why;
		if (!client.connect(kFeslHost, kFeslPort, 10, &why)) {
			if (error) {
				*error = why;
			}
			return std::nullopt;
		}
	}
	auto reply = client.hello();
	if (!reply || reply->isError()) {
		return fail("hello", reply);
	}
	reply = client.login(name, password);
	if (!reply || reply->isError()) {
		return fail("login", reply);
	}
	FeslSession session;
	session.account.displayName = reply->text("displayName");
	session.account.lkey = reply->text("lkey");
	session.account.userId = reply->number("userId");
	session.account.profileId = reply->number("profileId");
	reply = client.subAccounts();
	if (!reply || reply->isError()) {
		return fail("sub-accounts", reply);
	}
	session.account.soldiers = reply->list("subAccounts");
	session.soldier = soldier.empty() && !session.account.soldiers.empty() ? session.account.soldiers.front() : soldier;
	if (session.soldier.empty()) {
		if (error) {
			*error = "the account has no soldiers";
		}
		return std::nullopt;
	}
	reply = client.loginSubAccount(session.soldier);
	if (!reply || reply->isError()) {
		return fail("soldier login", reply);
	}
	session.soldierProfileId = reply->number("profileId");
	reply = client.gameSpyPreAuth();
	if (!reply || reply->isError()) {
		return fail("GameSpy pre-authentication", reply);
	}
	session.ticket = reply->text("ticket");
	return session;
}

} // namespace dice::hfe::net
