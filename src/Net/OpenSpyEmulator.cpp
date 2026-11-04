#include "OpenSpyEmulator.h"

#include "GameSpyCrypt.h"
#include "Md5.h"
#include "PlayerAuth.h"
#include "Socket.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>

namespace dice::hfe::net {

namespace {

enum class Service { Fesl, Gpcm, Master, Stella };

std::mt19937& rng()
{
	static std::mt19937 random {std::random_device {}()};
	return random;
}

std::string randomText(size_t length, const char* alphabet)
{
	const size_t n = std::strlen(alphabet);
	std::string out;
	for (size_t i = 0; i < length; ++i) {
		out += alphabet[rng()() % n];
	}
	return out;
}

std::string urlDecode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size()) {
			out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
			i += 2;
		} else if (s[i] == '+') {
			out += ' ';
		} else {
			out += s[i];
		}
	}
	return out;
}

std::string lower(std::string s)
{
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

} // namespace

struct OpenSpyEmulator::Listeners {
	TcpListener fesl, gpcm, master, stella;
	UdpServerSocket cdkey;
};

struct OpenSpyEmulator::Connection {
	Service service;
	TcpSocket socket;
	std::vector<uint8_t> bytes;
	std::string text;
	uint32_t sequence = 1;
	// FESL: who logged in.
	const EmulatorAccount* account = nullptr;
	long profileId = 0;
	std::chrono::steady_clock::time_point lastPing = std::chrono::steady_clock::now();
	// GP: the challenge sent.
	std::string challenge;
	bool closing = false;

	void send(const std::string& data) { socket.send(std::vector<uint8_t>(data.begin(), data.end())); }
	void send(const std::vector<uint8_t>& data) { socket.send(data); }
};

OpenSpyEmulator::OpenSpyEmulator() = default;
OpenSpyEmulator::~OpenSpyEmulator() { stop(); }

EmulatorAccount OpenSpyEmulator::account(const std::string& spec, long userId, long profileId)
{
	EmulatorAccount a;
	const size_t colon = spec.find(':');
	const size_t at = spec.rfind('@');
	a.name = spec.substr(0, colon);
	a.password = colon == std::string::npos ? std::string()
											: spec.substr(colon + 1, (at == std::string::npos || at < colon ? spec.size() : at) - colon - 1);
	a.userId = userId;
	a.profileId = profileId;
	std::string soldiers = at == std::string::npos || at < colon ? a.name : spec.substr(at + 1);
	std::stringstream list(soldiers);
	std::string nick;
	long id = profileId + 1;
	while (std::getline(list, nick, ',')) {
		a.soldiers.push_back({nick, id++});
	}
	return a;
}

bool OpenSpyEmulator::start(const OpenSpyEmulatorOptions& options, std::string* error)
{
	stop();
	m_options = options;
	m_listeners = std::make_unique<Listeners>();
	Listeners& l = *m_listeners;
	const auto fail = [&](const std::string& what, uint16_t port) {
		if (error) {
			*error = "cannot listen for " + what + " on 127.0.0.1:" + std::to_string(port);
		}
		m_listeners.reset();
		return false;
	};
	if (!l.fesl.listen(options.feslPort)) {
		return fail("FESL", options.feslPort);
	}
	if (!l.gpcm.listen(options.gpcmPort)) {
		return fail("GP", options.gpcmPort);
	}
	if (!l.master.listen(options.masterPort)) {
		return fail("the master server", options.masterPort);
	}
	if (!l.stella.listen(options.stellaPort)) {
		return fail("the stats service", options.stellaPort);
	}
	if (!l.cdkey.bind(options.cdkeyPort, true)) {
		return fail("the CD-key service", options.cdkeyPort);
	}
	m_running = true;
	m_thread = std::thread([this] { run(); });
	return true;
}

void OpenSpyEmulator::stop()
{
	m_running = false;
	if (m_thread.joinable()) {
		m_thread.join();
	}
	m_connections.clear();
	m_listeners.reset();
}

OnlineEndpoints OpenSpyEmulator::endpoints() const
{
	OnlineEndpoints e = localEndpoints("127.0.0.1");
	if (m_listeners) {
		e.fesl.port = m_listeners->fesl.port();
		e.gpcm.port = m_listeners->gpcm.port();
		e.master.port = m_listeners->master.port();
		e.stella.port = m_listeners->stella.port();
		e.cdkey.port = m_listeners->cdkey.port();
	}
	return e;
}

void OpenSpyEmulator::addServer(const ServerEntry& server)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_options.servers.push_back(server);
}

void OpenSpyEmulator::log(const std::string& text)
{
	if (onLog) {
		onLog(text);
	}
}

const EmulatorSoldier* OpenSpyEmulator::soldier(long profileId, const EmulatorAccount** owner) const
{
	for (const EmulatorAccount& a : m_options.accounts) {
		for (const EmulatorSoldier& s : a.soldiers) {
			if (s.profileId == profileId) {
				if (owner) {
					*owner = &a;
				}
				return &s;
			}
		}
	}
	return nullptr;
}

void OpenSpyEmulator::run()
{
	Listeners& l = *m_listeners;
	while (m_running) {
		const std::pair<TcpListener*, Service> listeners[] = {
			{&l.fesl, Service::Fesl}, {&l.gpcm, Service::Gpcm}, {&l.master, Service::Master}, {&l.stella, Service::Stella}};
		for (const auto& [listener, service] : listeners) {
			auto c = std::make_unique<Connection>();
			if (listener->accept(c->socket, 0)) {
				c->service = service;
				if (service == Service::Gpcm) {
					// GPPeer.cpp: the login challenge on connecting.
					c->challenge = randomText(10, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
					c->send(encodeGp({{"lc", "1"}, {"challenge", c->challenge}, {"id", "1"}}));
				}
				m_connections.push_back(std::move(c));
			}
		}
		cdkeyDatagram();
		for (auto& c : m_connections) {
			const int got = c->socket.receiveFor(c->bytes, 0);
			if (got < 0) {
				c->closing = true;
			}
			switch (c->service) {
			case Service::Fesl: {
				bool bad = false;
				for (const FeslMessage& m : takeFeslMessages(c->bytes, &bad)) {
					feslMessage(*c, m);
				}
				if (bad) {
					c->closing = true;
				}
				const auto now = std::chrono::steady_clock::now();
				if (now - c->lastPing > std::chrono::seconds(m_options.pingSeconds)) {
					c->lastPing = now;
					c->send(encodeFesl({kFeslSystem, 0x80000000 | c->sequence++, {{"TXN", "Ping"}, {"TID", "0"}}}));
				}
				break;
			}
			case Service::Gpcm:
				c->text.append(c->bytes.begin(), c->bytes.end());
				c->bytes.clear();
				for (const GpMessage& m : takeGpMessages(c->text)) {
					gpMessage(*c, m);
				}
				break;
			case Service::Master:
				masterRequest(*c);
				break;
			case Service::Stella:
				stellaRequest(*c);
				break;
			}
		}
		for (size_t i = 0; i < m_connections.size();) {
			if (m_connections[i]->closing) {
				m_connections.erase(m_connections.begin() + static_cast<std::ptrdiff_t>(i));
			} else {
				++i;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

void OpenSpyEmulator::feslMessage(Connection& c, const FeslMessage& m)
{
	const std::string txn = m.txn();
	const std::string tid = m.get("TID");
	log("fesl: " + txn);
	std::vector<std::pair<std::string, std::string>> out {{"TXN", txn}};
	if (!tid.empty()) {
		out.emplace_back("TID", tid);
	}
	const auto reply = [&](uint32_t type) { c.send(encodeFesl({type, 0x80000000 | c.sequence++, out})); };
	const auto error = [&](long code) {
		out.emplace_back("errorContainer", "[]");
		out.emplace_back("errorCode", std::to_string(code));
		reply(kFeslAccount);
	};
	if (m.type == kFeslSystem) {
		if (txn == "Hello") {
			char when[64];
			const std::time_t now = std::time(nullptr);
			std::strftime(when, sizeof(when), "%b-%d-%Y %H:%M:%S UTC", std::gmtime(&now));
			std::string encoded;
			for (const char ch : std::string(when)) {
				encoded += ch == ' ' ? std::string("%20") : ch == ':' ? std::string("%3a") : std::string(1, ch);
			}
			out.insert(out.end(), {{"domainPartition.domain", "eagames"}, {"messengerIp", "127.0.0.1"}, {"messengerPort", "0"},
									  {"domationPartition.subDomain", "bf2142"}, {"activityTimeoutSecs", std::to_string(m_options.pingSeconds * 2)},
									  {"curTime", "\"" + encoded + "\""}, {"theaterIp", "127.0.0.1"}, {"theaterPort", "0"}});
			reply(kFeslSystem);
			// FESLPeer.cpp send_memcheck: sequence forced to 0.
			c.send(encodeFesl({kFeslSystem, 0x80000000, {{"TXN", "MemCheck"}, {"memcheck.[]", "0"}, {"type", "0"},
														  {"salt", std::to_string(now)}}}));
		} else if (txn == "Goodbye") {
			c.closing = true;
		}
		return; // Ping and MemCheck answers need nothing
	}
	if (m.type != kFeslAccount) {
		return;
	}
	if (txn == "Login") {
		const std::string name = m.text("name"), password = m.text("password");
		for (const EmulatorAccount& a : m_options.accounts) {
			if (a.name == name) {
				if (a.password != password) {
					error(122);
					return;
				}
				c.account = &a;
				c.profileId = 0;
				out.insert(out.end(), {{"lkey", randomText(27, "abcdefghijklmnopqrstuvwxyz0123456789")}, {"displayName", a.name},
										  {"userId", std::to_string(a.userId)}, {"profileId", std::to_string(a.profileId)}});
				reply(kFeslAccount);
				return;
			}
		}
		error(101);
	} else if (txn == "GetSubAccounts") {
		if (!c.account) {
			error(122);
			return;
		}
		out.emplace_back("subAccounts.[]", std::to_string(c.account->soldiers.size()));
		for (size_t i = 0; i < c.account->soldiers.size(); ++i) {
			out.emplace_back("subAccounts." + std::to_string(i), "\"" + c.account->soldiers[i].nick + "\"");
		}
		reply(kFeslAccount);
	} else if (txn == "LoginSubAccount") {
		if (c.account) {
			for (const EmulatorSoldier& s : c.account->soldiers) {
				if (s.nick == m.text("name")) {
					c.profileId = s.profileId;
					out.insert(out.end(), {{"lkey", randomText(27, "abcdefghijklmnopqrstuvwxyz0123456789")},
											  {"profileId", std::to_string(s.profileId)}, {"userId", std::to_string(c.account->userId)}});
					reply(kFeslAccount);
					return;
				}
			}
		}
		error(101);
	} else if (txn == "GameSpyPreAuth") {
		if (!c.account) {
			error(122);
			return;
		}
		const long profile = c.profileId ? c.profileId : c.account->profileId;
		const std::string challenge = randomText(8, "abcdefghijklmnopqrstuvwxyz");
		const std::string ticket = randomText(32, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_tickets[ticket] = {profile, challenge};
		}
		out.emplace_back("challenge", challenge);
		out.emplace_back("ticket", ticket);
		reply(kFeslAccount);
	} else {
		error(99);
	}
}

void OpenSpyEmulator::gpMessage(Connection& c, const GpMessage& m)
{
	if (m.empty()) {
		return;
	}
	const std::string command = m.front().first;
	log("gpcm: " + command);
	if (command == "logout") {
		c.closing = true;
		return;
	}
	if (command != "login") {
		return; // ka, status: nothing to answer
	}
	const std::string id = gpValue(m, "id");
	const auto error = [&](int code, const std::string& text) {
		c.send(encodeGp({{"error", ""}, {"err", std::to_string(code)}, {"fatal", ""}, {"errmsg", text}, {"id", id}}));
	};
	const std::string token = gpValue(m, "authtoken");
	std::pair<long, std::string> ticket;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto it = m_tickets.find(token);
		if (token.empty() || it == m_tickets.end()) {
			error(265, "Invalid authentication token.");
			return;
		}
		ticket = it->second;
	}
	const std::string clientChallenge = gpValue(m, "challenge");
	if (gpValue(m, "response").substr(0, 32) != gpLoginResponse(ticket.second, token, clientChallenge, c.challenge)) {
		error(260, "The password provided is incorrect.");
		return;
	}
	const EmulatorAccount* owner = nullptr;
	const EmulatorSoldier* s = soldier(ticket.first, &owner);
	std::string nick = s ? s->nick : std::string();
	long userId = owner ? owner->userId : 0;
	if (!s) {
		for (const EmulatorAccount& a : m_options.accounts) {
			if (a.profileId == ticket.first) {
				nick = a.name;
				userId = a.userId;
			}
		}
	}
	std::string lt;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		lt = randomText(24, "0123456789abcdef");
		m_sessions[lt] = ticket.first;
	}
	++m_gpLogins;
	c.send(encodeGp({{"lc", "2"}, {"sesskey", std::to_string(m_nextSession++)}, {"userid", std::to_string(userId)},
		{"profileid", std::to_string(ticket.first)}, {"uniquenick", nick}, {"lt", lt},
		{"proof", gpLoginProof(ticket.second, token, clientChallenge, c.challenge)}, {"id", id}}));
	c.send(encodeGp({{"bdy", "0"}, {"list", ""}}));
	c.send(encodeGp({{"blk", "0"}, {"list", ""}}));
}

void OpenSpyEmulator::masterRequest(Connection& c)
{
	// The list request (ServerBrowser.h): u16 length, 0, 1, 3, u32, two
	// game names, the 8-byte challenge, the filter, the fields, u32.
	if (c.bytes.size() < 2) {
		return;
	}
	const size_t length = static_cast<size_t>(c.bytes[0]) << 8 | c.bytes[1];
	if (c.bytes.size() < length || length < 9) {
		return;
	}
	const std::vector<uint8_t> request(c.bytes.begin(), c.bytes.begin() + static_cast<std::ptrdiff_t>(length));
	c.bytes.erase(c.bytes.begin(), c.bytes.begin() + static_cast<std::ptrdiff_t>(length));
	size_t at = 9;
	const auto cstring = [&](std::string& out) {
		out.clear();
		while (at < request.size() && request[at] != 0) {
			out += static_cast<char>(request[at++]);
		}
		++at;
	};
	std::string game, from, filter, fieldList;
	cstring(game);
	cstring(from);
	if (at + 8 > request.size()) {
		c.closing = true;
		return;
	}
	std::array<uint8_t, 8> challenge {};
	std::memcpy(challenge.data(), request.data() + at, 8);
	at += 8;
	cstring(filter);
	cstring(fieldList);
	log("master: list for " + game + " (" + fieldList + ")");
	std::vector<std::string> fields;
	std::stringstream split(fieldList);
	std::string field;
	while (std::getline(split, field, '\\')) {
		if (!field.empty()) {
			fields.push_back(field);
		}
	}
	// The plain reply.
	std::vector<uint8_t> plain {127, 0, 0, 1, 0x1a, 0xa6}; // our IP as the master sees it, query port 6790
	plain.push_back(static_cast<uint8_t>(fields.size()));
	for (const std::string& f : fields) {
		plain.push_back(0); // strings
		plain.insert(plain.end(), f.begin(), f.end());
		plain.push_back(0);
	}
	plain.push_back(0); // no popular values
	std::vector<ServerEntry> servers;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		servers = m_options.servers;
	}
	for (const ServerEntry& s : servers) {
		plain.push_back(0x10 | 0x40); // its own port, with keys
		int a = 0, b = 0, cc = 0, d = 0;
		std::sscanf(s.ip.c_str(), "%d.%d.%d.%d", &a, &b, &cc, &d);
		plain.insert(plain.end(), {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(cc), static_cast<uint8_t>(d)});
		plain.push_back(static_cast<uint8_t>(s.port >> 8));
		plain.push_back(static_cast<uint8_t>(s.port));
		for (const std::string& f : fields) {
			plain.push_back(0xff);
			const auto it = s.fields.find(f);
			const std::string value = it == s.fields.end() ? std::string() : it->second;
			plain.insert(plain.end(), value.begin(), value.end());
			plain.push_back(0);
		}
	}
	plain.insert(plain.end(), {0, 255, 255, 255, 255});
	// The header: one byte (length 2) and the server challenge's length.
	const std::string serverKey = randomText(14, "abcdefghijklmnopqrstuvwxyz");
	std::vector<uint8_t> reply {0xec, static_cast<uint8_t>(serverKey.size() ^ 0xea)};
	reply.insert(reply.end(), serverKey.begin(), serverKey.end());
	std::array<uint8_t, 8> key = challenge;
	const size_t secretLength = std::strlen(kSecretKey);
	for (size_t i = 0; i < serverKey.size(); ++i) {
		const size_t k = (static_cast<size_t>(static_cast<int>(static_cast<signed char>(kSecretKey[i % secretLength])) * static_cast<int>(i)) & 7);
		key[k] = static_cast<uint8_t>(key[k] ^ key[i & 7] ^ static_cast<uint8_t>(serverKey[i]));
	}
	GameSpyCrypt crypt(key.data(), key.size());
	for (uint8_t byte : plain) {
		reply.push_back(crypt.encrypt(byte));
	}
	c.send(reply);
	c.closing = true;
}

void OpenSpyEmulator::stellaRequest(Connection& c)
{
	const std::string text(c.bytes.begin(), c.bytes.end());
	const size_t end = text.find("\r\n\r\n");
	if (end == std::string::npos) {
		return;
	}
	c.bytes.clear();
	const size_t space = text.find(' ');
	const std::string target = text.substr(space + 1, text.find(' ', space + 1) - space - 1);
	const size_t q = target.find('?');
	const std::string path = lower(target.substr(0, q));
	std::map<std::string, std::string> query;
	if (q != std::string::npos) {
		std::stringstream params(target.substr(q + 1));
		std::string pair;
		while (std::getline(params, pair, '&')) {
			const size_t eq = pair.find('=');
			query[pair.substr(0, eq)] = eq == std::string::npos ? std::string() : urlDecode(pair.substr(eq + 1));
		}
	}
	log("stella: " + target);
	std::string body;
	if (path == "/validateplayer.aspx") {
		++m_validations;
		// API/requests/ValidatePlayer.js.
		long session = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto it = m_sessions.find(query["auth"]);
			session = it == m_sessions.end() ? 0 : it->second;
		}
		const EmulatorSoldier* s = session ? soldier(session) : nullptr;
		const std::string nick = query["SoldierNick"];
		std::string pid = "0", realNick = ("INVALID " + nick).substr(0, 23), result = "InvalidAuthProfileID";
		if (s) {
			pid = std::to_string(s->profileId);
			realNick = s->nick;
			result = s->nick != nick ? "InvalidReportedNick" : pid != query["pid"] ? "InvalidReportedProfileID" : "Ok";
		}
		const std::string data = "O\nH\tpid\tnick\tspid\tasof\t\nD\t" + pid + "\t" + realNick + "\t" + query["pid"] + "\t"
			+ std::to_string(std::time(nullptr)) + "\t\nH\tresult\t\nD\t" + result + "\t\n";
		body = data + "$\t" + std::to_string(data.size()) + "\t$\n";
	} else {
		body = "E\t999\n$\t4\t$\n";
	}
	c.send("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size())
		+ "\r\nConnection: close\r\n\r\n" + body);
	c.closing = true;
}

void OpenSpyEmulator::cdkeyDatagram()
{
	std::vector<uint8_t> datagram;
	UdpServerSocket::Address from;
	if (m_listeners->cdkey.receiveFrom(datagram, from, 0) <= 0) {
		return;
	}
	const std::vector<uint8_t> plain = gameSpyXor(std::string(datagram.begin(), datagram.end()));
	const GpMessage m = [&] {
		std::string text(plain.begin(), plain.end());
		text += "\\final\\";
		auto messages = takeGpMessages(text);
		return messages.empty() ? GpMessage() : messages.front();
	}();
	if (m.empty() || m.front().first != "auth") {
		return;
	}
	const std::string response = gpValue(m, "resp");
	const std::string skey = gpValue(m, "skey");
	log("cdkey: auth " + response.substr(0, 32));
	bool ok = m_options.cdKeys.empty();
	for (const std::string& key : m_options.cdKeys) {
		// 0x00719260's response: md5(key), %.8x of the number, and
		// md5(key + number % 0xffff + challenge).
		if (response.size() < 72 || response.substr(0, 32) != md5Hex(key)) {
			continue;
		}
		const uint32_t number = static_cast<uint32_t>(std::strtoul(response.substr(32, 8).c_str(), nullptr, 16));
		if (response.substr(40, 32) == md5Hex(key + std::to_string(number % 0xffff) + gpValue(m, "ch"))) {
			ok = true;
		}
	}
	const std::string answer = ok ? "\\uok\\\\cd\\" + response.substr(0, 32) + "\\skey\\" + skey
								  : "\\unok\\\\cd\\" + response.substr(0, 32) + "\\skey\\" + skey + "\\errmsg\\Invalid CD Key";
	m_listeners->cdkey.sendTo(from, gameSpyXor(answer));
}

} // namespace dice::hfe::net
