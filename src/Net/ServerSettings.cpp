#include "ServerSettings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace dice::hfe::net {

namespace {

// One setting: its console name, how sv.save writes it, and its member.
struct Field {
	const char* name;
	bool quoted; // written "in quotes" by 0x00431880
	bool saved;
	std::function<std::string(const ServerSettings&)> get;
	std::function<void(ServerSettings&, const std::string&)> set;
};

template <class T>
std::string format(const T& value)
{
	std::ostringstream out;
	if constexpr (std::is_same_v<T, bool>) {
		out << (value ? 1 : 0);
	} else {
		out << value;
	}
	return out.str();
}

template <class T>
T parse(const std::string& text, const T& fallback)
{
	if constexpr (std::is_same_v<T, std::string>) {
		return text;
	} else if constexpr (std::is_same_v<T, bool>) {
		std::istringstream in(text);
		int number = 0;
		return in >> number ? number != 0 : fallback;
	} else {
		std::istringstream in(text);
		T value {};
		return in >> value ? value : fallback; // a value that can't be parsed keeps the old one
	}
}

template <class T>
Field field(const char* name, T ServerSettings::*member, bool quoted = false, bool saved = true)
{
	return {name, quoted, saved, [member](const ServerSettings& s) { return format(s.*member); },
		[member](ServerSettings& s, const std::string& text) { s.*member = parse(text, s.*member); }};
}

const std::vector<Field>& fields()
{
	using S = ServerSettings;
	static const std::vector<Field> table {
		field("serverName", &S::serverName, true),
		field("password", &S::password, true),
		field("internet", &S::internet),
		field("bandwidthChoke", &S::bandwidthChoke),
		field("serverIP", &S::serverIP, true),
		field("serverPort", &S::serverPort),
		field("welcomeMessage", &S::welcomeMessage, true),
		field("punkBuster", &S::punkBuster),
		field("allowFreeCam", &S::allowFreeCam),
		field("allowExternalViews", &S::allowExternalViews),
		field("allowNoseCam", &S::allowNoseCam),
		field("hitIndicator", &S::hitIndicator),
		field("maxPlayers", &S::maxPlayers),
		field("numPlayersNeededToStart", &S::numPlayersNeededToStart),
		field("notEnoughPlayersRestartDelay", &S::notEnoughPlayersRestartDelay),
		field("startDelay", &S::startDelay),
		field("endDelay", &S::endDelay),
		field("spawnTime", &S::spawnTime),
		field("manDownTime", &S::manDownTime),
		field("endOfRoundDelay", &S::endOfRoundDelay),
		field("ticketRatio", &S::ticketRatio),
		field("roundsPerMap", &S::roundsPerMap),
		field("timeLimit", &S::timeLimit),
		field("scoreLimit", &S::scoreLimit),
		field("soldierFriendlyFire", &S::soldierFriendlyFire),
		field("vehicleFriendlyFire", &S::vehicleFriendlyFire),
		field("soldierSplashFriendlyFire", &S::soldierSplashFriendlyFire),
		field("vehicleSplashFriendlyFire", &S::vehicleSplashFriendlyFire),
		field("tkPunishEnabled", &S::tkPunishEnabled),
		field("tkNumPunishToKick", &S::tkNumPunishToKick),
		field("tkPunishByDefault", &S::tkPunishByDefault),
		field("votingEnabled", &S::votingEnabled),
		field("voteTime", &S::voteTime),
		field("minPlayersForVoting", &S::minPlayersForVoting),
		field("gameSpyPort", &S::gameSpyPort),
		field("allowNATNegotiation", &S::allowNATNegotiation),
		field("interfaceIP", &S::interfaceIP),
		field("autoRecord", &S::autoRecord),
		field("demoIndexURL", &S::demoIndexURL, true),
		field("demoDownloadURL", &S::demoDownloadURL, true),
		field("autoDemoHook", &S::autoDemoHook, true),
		field("demoQuality", &S::demoQuality),
		field("adminScript", &S::adminScript, true),
		field("timeBeforeRestartMap", &S::timeBeforeRestartMap),
		field("autoBalanceTeam", &S::autoBalanceTeam),
		field("teamRatioPercent", &S::teamRatioPercent),
		field("voipEnabled", &S::voipEnabled),
		field("voipQuality", &S::voipQuality),
		field("voipServerRemote", &S::voipServerRemote),
		field("voipServerRemoteIP", &S::voipServerRemoteIP),
		field("voipServerPort", &S::voipServerPort),
		field("voipBFClientPort", &S::voipBFClientPort),
		field("voipBFServerPort", &S::voipBFServerPort),
		field("voipSharedPassword", &S::voipSharedPassword),
		field("useGlobalRank", &S::useGlobalRank),
		field("useGlobalUnlocks", &S::useGlobalUnlocks),
		field("ranked", &S::ranked), // written only for a ranked server
		field("minUnlockLevel", &S::minUnlockLevel),
		field("maxUnlockLevel", &S::maxUnlockLevel),
		field("sponsorText", &S::sponsorText, true),
		field("sponsorLogoURL", &S::sponsorLogoURL, true),
		field("communityLogoURL", &S::communityLogoURL, true),
		field("customMapURL", &S::customMapURL, true),
		field("radioSpamInterval", &S::radioSpamInterval),
		field("radioMaxSpamFlagCount", &S::radioMaxSpamFlagCount),
		field("radioBlockedDurationTime", &S::radioBlockedDurationTime),
		field("numReservedSlots", &S::numReservedSlots),
		field("maxRank", &S::maxRank),
		field("friendlyFireWithMines", &S::friendlyFireWithMines),
		field("botSkill", &S::botSkill),
		field("allowSpectators", &S::allowSpectators),
		field("allowTitanMovement", &S::allowTitanMovement),
		field("configFile", &S::configFile, false, false),
		field("maxConnectionType", &S::maxConnectionType, false, false),
	};
	return table;
}

bool sameName(const std::string& a, const char* b)
{
	size_t i = 0;
	for (; i < a.size() && b[i]; ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return i == a.size() && !b[i];
}

std::string quote(const std::string& text) { return "\"" + text + "\""; }

// The address part of "a.b.c.d:port".
std::string hostOf(const std::string& address) { return address.substr(0, address.find(':')); }

} // namespace

std::string ServerSettings::save() const
{
	std::string out;
	for (const Field& f : fields()) {
		if (!f.saved || (std::string(f.name) == "ranked" && !ranked)) {
			continue;
		}
		const std::string value = f.get(*this);
		out += "sv." + std::string(f.name) + " " + (f.quoted ? quote(value) : value) + "\n";
	}
	return out;
}

bool ServerSettings::get(const std::string& name, std::string& value) const
{
	for (const Field& f : fields()) {
		if (sameName(name, f.name)) {
			value = f.get(*this);
			return true;
		}
	}
	return false;
}

bool ServerSettings::set(const std::string& name, const std::string& value)
{
	for (const Field& f : fields()) {
		if (sameName(name, f.name)) {
			f.set(*this, value);
			return true;
		}
	}
	return false;
}

const std::vector<std::string>& ServerSettings::names()
{
	static const std::vector<std::string> list = [] {
		std::vector<std::string> out;
		for (const Field& f : fields()) {
			out.push_back(f.name);
		}
		return out;
	}();
	return list;
}

// --- MapList

bool MapList::append(const MapListEntry& entry, std::string* error)
{
	return insert(count(), entry, error);
}

bool MapList::insert(int index, const MapListEntry& entry, std::string* error)
{
	if (index < 0 || index > count()) {
		if (error) {
			*error = "index out of range";
		}
		return false;
	}
	std::string why;
	if (validate && !validate(entry, why)) {
		if (error) {
			*error = why;
		}
		return false;
	}
	m_entries.insert(m_entries.begin() + index, entry);
	if (index <= m_current && count() > 1) {
		++m_current; // the running map stays the running map
	}
	if (m_next >= index && m_next >= 0) {
		++m_next;
	}
	return true;
}

int MapList::remove(int index)
{
	if (index < 0 || index >= count()) {
		return -1;
	}
	m_entries.erase(m_entries.begin() + index);
	if (index < m_current) {
		--m_current;
	}
	if (m_current >= count()) {
		m_current = 0;
	}
	if (m_next == index) {
		m_next = -1;
	} else if (m_next > index) {
		--m_next;
	}
	return count();
}

void MapList::clear()
{
	m_entries.clear();
	m_current = 0;
	m_next = -1;
}

std::string MapList::list() const
{
	std::string out;
	for (size_t i = 0; i < m_entries.size(); ++i) {
		const MapListEntry& e = m_entries[i];
		out += std::to_string(i) + ": " + quote(e.level) + " " + quote(e.gameMode) + " " + std::to_string(e.size) + "\n";
	}
	return out;
}

std::string MapList::save() const
{
	// 0x00440ef0: maplist.append "level" "mode" size
	std::string out;
	for (const MapListEntry& e : m_entries) {
		out += "maplist.append " + quote(e.level) + " " + quote(e.gameMode) + " " + std::to_string(e.size) + "\n";
	}
	return out;
}

void MapList::setCurrent(int index)
{
	if (index >= 0 && index < count()) {
		m_current = index;
	}
}

int MapList::next() const
{
	if (m_entries.empty()) {
		return 0;
	}
	if (m_next >= 0 && m_next < count()) {
		return m_next;
	}
	return (m_current + 1) % count();
}

void MapList::setNext(int index)
{
	if (index >= 0 && index < count()) {
		m_next = index;
	}
}

void MapList::advance()
{
	if (m_entries.empty()) {
		return;
	}
	m_current = next();
	m_next = -1;
}

// --- bans

std::string Ban::period() const
{
	switch (kind) {
	case Kind::Permanent:
		return "perm";
	case Kind::Round:
		return "round";
	case Kind::Until:
		break;
	}
	return std::to_string(static_cast<long long>(until));
}

bool BanList::parsePeriod(const std::string& text, Ban& out, std::time_t now)
{
	// 0x00505db0: "" and "perm" for good, "round" for this round, else a
	// number of seconds (no '-').
	std::string lower = text;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	if (lower.empty() || lower == "perm") {
		out.kind = Ban::Kind::Permanent;
		return true;
	}
	if (lower == "round") {
		out.kind = Ban::Kind::Round;
		return true;
	}
	if (lower.find('-') != std::string::npos || lower.find_first_not_of("0123456789") != std::string::npos) {
		return false;
	}
	const long long seconds = std::atoll(lower.c_str());
	out.kind = Ban::Kind::Until;
	// A saved ban carries the time it ends (more than a year of seconds).
	out.until = seconds > 400000000 ? static_cast<std::time_t>(seconds) : now + static_cast<std::time_t>(seconds);
	return true;
}

namespace {

bool addBan(std::vector<Ban>& list, const std::string& what, const std::string& period, std::time_t now)
{
	Ban ban;
	ban.what = what;
	if (what.empty() || !BanList::parsePeriod(period, ban, now)) {
		return false;
	}
	list.erase(std::remove_if(list.begin(), list.end(), [&](const Ban& b) { return b.what == what; }), list.end());
	list.push_back(ban);
	return true;
}

bool active(const Ban& ban, std::time_t now) { return ban.kind != Ban::Kind::Until || ban.until > now; }

std::string listBans(const std::vector<Ban>& list, std::time_t now)
{
	std::string out;
	for (const Ban& ban : list) {
		if (active(ban, now)) {
			out += ban.what + " " + ban.period() + "\n";
		}
	}
	return out;
}

} // namespace

bool BanList::addAddress(const std::string& address, const std::string& period, std::time_t now)
{
	return addBan(m_addresses, hostOf(address), period, now);
}

bool BanList::addKey(const std::string& key, const std::string& period, std::time_t now)
{
	return addBan(m_keys, key, period, now);
}

void BanList::removeAddress(const std::string& address)
{
	const std::string host = hostOf(address);
	m_addresses.erase(std::remove_if(m_addresses.begin(), m_addresses.end(), [&](const Ban& b) { return b.what == host; }),
		m_addresses.end());
}

void BanList::removeKey(const std::string& key)
{
	m_keys.erase(std::remove_if(m_keys.begin(), m_keys.end(), [&](const Ban& b) { return b.what == key; }), m_keys.end());
}

void BanList::clear()
{
	m_addresses.clear();
	m_keys.clear();
}

bool BanList::bannedAddress(const std::string& address, std::time_t now) const
{
	const std::string host = hostOf(address);
	return std::any_of(m_addresses.begin(), m_addresses.end(), [&](const Ban& b) { return b.what == host && active(b, now); });
}

bool BanList::bannedKey(const std::string& key, std::time_t now) const
{
	return !key.empty()
		&& std::any_of(m_keys.begin(), m_keys.end(), [&](const Ban& b) { return b.what == key && active(b, now); });
}

void BanList::endRound()
{
	for (std::vector<Ban>* list : {&m_addresses, &m_keys}) {
		list->erase(std::remove_if(list->begin(), list->end(), [](const Ban& b) { return b.kind == Ban::Kind::Round; }),
			list->end());
	}
}

std::string BanList::listAddresses(std::time_t now) const { return listBans(m_addresses, now); }
std::string BanList::listKeys(std::time_t now) const { return listBans(m_keys, now); }

std::string BanList::save(std::time_t now) const
{
	// 0x00507570: the round bans aren't kept.
	std::string out;
	for (const Ban& ban : m_keys) {
		if (ban.kind != Ban::Kind::Round && active(ban, now)) {
			out += "admin.addKeyToBanList " + ban.what + " " + ban.period() + "\n";
		}
	}
	for (const Ban& ban : m_addresses) {
		if (ban.kind != Ban::Kind::Round && active(ban, now)) {
			out += "admin.addAddressToBanList " + ban.what + " " + ban.period() + "\n";
		}
	}
	return out;
}

// --- reserved slots

void ReservedSlots::addNick(const std::string& nick)
{
	if (!nick.empty() && !has(nick)) {
		m_nicks.push_back(nick);
	}
}

void ReservedSlots::removeNick(const std::string& nick)
{
	m_nicks.erase(std::remove(m_nicks.begin(), m_nicks.end(), nick), m_nicks.end());
}

bool ReservedSlots::has(const std::string& nick) const
{
	return std::find(m_nicks.begin(), m_nicks.end(), nick) != m_nicks.end();
}

std::string ReservedSlots::list() const
{
	std::string out;
	for (const std::string& nick : m_nicks) {
		out += nick + "\n";
	}
	return out;
}

std::string ReservedSlots::save() const
{
	std::string out;
	for (const std::string& nick : m_nicks) {
		out += "reservedSlots.addNick " + quote(nick) + "\n";
	}
	return out;
}

} // namespace dice::hfe::net
