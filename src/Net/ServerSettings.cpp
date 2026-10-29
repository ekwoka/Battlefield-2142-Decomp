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

// The sv.* rows (the GameServerSettings console object, 0x0041aa74..
// 0x00422035): each field's row constructor, then its executors (the
// property's get and set), as cited by its line.
const std::vector<Field>& fields()
{
	using S = ServerSettings;
	static const std::vector<Field> table {
		field("serverName", &S::serverName, true), // 0x0041b275 0x0041b2f2 0x0041b35c
		field("password", &S::password, true), // 0x0041b3d1 0x0041b450 0x0041b4c5 0x0041b556
		field("internet", &S::internet), // 0x0041b59b 0x0041b61c 0x0041b669 0x0041b6de
		field("bandwidthChoke", &S::bandwidthChoke), // 0x0041b721 0x0041b792 0x0041b7df
		field("serverIP", &S::serverIP, true), // 0x0041bad7 0x0041bb54 0x0041bbbe
		field("serverPort", &S::serverPort), // 0x0041bc33 0x0041bcb4 0x0041bd01 0x0041bd71
		field("welcomeMessage", &S::welcomeMessage, true), // 0x0041b97d 0x0041b9fa 0x0041ba62
		field("punkBuster", &S::punkBuster), // 0x004207a8 0x00420819 0x00420866 0x004208ae 0x0042093a 0x00420946 0x00420968
		field("allowFreeCam", &S::allowFreeCam), // 0x0041bdb6 0x0041be27 0x0041be74
		field("allowExternalViews", &S::allowExternalViews), // 0x0041bec8 0x0041bf39 0x0041bf86
		field("allowNoseCam", &S::allowNoseCam), // 0x0041bfda 0x0041c04b 0x0041c098
		field("hitIndicator", &S::hitIndicator), // 0x0041c293 0x0041c304 0x0041c351
		field("maxPlayers", &S::maxPlayers), // 0x0041c3a5 0x0041c426 0x0041c473 0x0041c4e3
		field("numPlayersNeededToStart", &S::numPlayersNeededToStart), // 0x0041c834 0x0041c8a5 0x0041c8f2
		field("notEnoughPlayersRestartDelay", &S::notEnoughPlayersRestartDelay), // 0x0041c93a 0x0041c9ab 0x0041c9f8
		field("startDelay", &S::startDelay), // 0x0041ca4b 0x0041cabc 0x0041cb09
		field("endDelay", &S::endDelay), // 0x0041cb53 0x0041cbc4 0x0041cc11
		field("spawnTime", &S::spawnTime), // 0x0041cc5b 0x0041cccc 0x0041cd19
		field("manDownTime", &S::manDownTime), // 0x0041cd6b 0x0041cddc 0x0041ce29
		field("endOfRoundDelay", &S::endOfRoundDelay), // 0x0041ce7b 0x0041ceec 0x0041cf39
		field("ticketRatio", &S::ticketRatio), // 0x0041cf81 0x0041cff2 0x0041d03f
		field("roundsPerMap", &S::roundsPerMap), // 0x0041d086 0x0041d0f7 0x0041d144
		field("timeLimit", &S::timeLimit), // 0x0041d18e 0x0041d1ff 0x0041d24c
		field("scoreLimit", &S::scoreLimit), // 0x0041d296 0x0041d307 0x0041d354
		field("soldierFriendlyFire", &S::soldierFriendlyFire), // 0x0041d39e 0x0041d40f 0x0041d45c
		field("vehicleFriendlyFire", &S::vehicleFriendlyFire), // 0x0041d4b6 0x0041d527 0x0041d574
		field("soldierSplashFriendlyFire", &S::soldierSplashFriendlyFire), // 0x0041d5ce 0x0041d63f 0x0041d68c
		field("vehicleSplashFriendlyFire", &S::vehicleSplashFriendlyFire), // 0x0041d6e6 0x0041d757 0x0041d7a4
		field("tkPunishEnabled", &S::tkPunishEnabled), // 0x0041d921 0x0041d992 0x0041d9df
		field("tkNumPunishToKick", &S::tkNumPunishToKick), // 0x0041da29 0x0041da9a 0x0041dae7
		field("tkPunishByDefault", &S::tkPunishByDefault), // 0x0041db31 0x0041dba2 0x0041dbef
		field("votingEnabled", &S::votingEnabled), // 0x0041dc43 0x0041dcb4 0x0041dd01
		field("voteTime", &S::voteTime), // 0x0041dd55 0x0041ddc6 0x0041de13
		field("minPlayersForVoting", &S::minPlayersForVoting), // 0x0041de5d 0x0041dece 0x0041df1b
		field("gameSpyPort", &S::gameSpyPort), // 0x0041e883 0x0041e8f4 0x0041e941
		field("allowNATNegotiation", &S::allowNATNegotiation), // 0x0041e98b 0x0041e9fc 0x0041ea49
		field("interfaceIP", &S::interfaceIP), // 0x0041ea93 0x0041eb10 0x0041eb7f
		field("autoRecord", &S::autoRecord), // 0x0041f223 0x0041f294 0x0041f2e1
		field("demoIndexURL", &S::demoIndexURL, true), // 0x0041f32b 0x0041f3a8 0x0041f417
		field("demoDownloadURL", &S::demoDownloadURL, true), // 0x0041f48c 0x0041f509 0x0041f578
		field("autoDemoHook", &S::autoDemoHook, true), // 0x0041f5ed 0x0041f66a 0x0041f6d9
		field("demoQuality", &S::demoQuality), // 0x0041f74e 0x0041f7bf 0x0041f80c
		field("adminScript", &S::adminScript, true), // 0x0041f868 0x0041f8e5 0x0041f954
		field("timeBeforeRestartMap", &S::timeBeforeRestartMap), // 0x0041ebf4 0x0041ec65 0x0041ecb2
		field("autoBalanceTeam", &S::autoBalanceTeam), // 0x0041ed05 0x0041ed76 0x0041edc3
		field("teamRatioPercent", &S::teamRatioPercent), // 0x0041ee0d 0x0041ee7e 0x0041eecb
		field("voipEnabled", &S::voipEnabled), // 0x0041df65 0x0041dfd6 0x0041e023
		field("voipQuality", &S::voipQuality), // 0x0041e77b 0x0041e7ec 0x0041e839
		field("voipServerRemote", &S::voipServerRemote), // 0x0041e06d 0x0041e0de 0x0041e12b
		field("voipServerRemoteIP", &S::voipServerRemoteIP), // 0x0041e175 0x0041e1f2 0x0041e261
		field("voipServerPort", &S::voipServerPort), // 0x0041e2d6 0x0041e347 0x0041e394
		field("voipBFClientPort", &S::voipBFClientPort), // 0x0041e3de 0x0041e44f 0x0041e49c
		field("voipBFServerPort", &S::voipBFServerPort), // 0x0041e4e6 0x0041e557 0x0041e5a4
		field("voipSharedPassword", &S::voipSharedPassword), // 0x0041e5ee 0x0041e66b 0x0041e706
		field("useGlobalRank", &S::useGlobalRank), // 0x00420273 0x004202e4 0x00420331
		field("useGlobalUnlocks", &S::useGlobalUnlocks), // 0x00420379 0x004203ea 0x00420437
		field("ranked", &S::ranked), // 0x00420697 0x00420708 0x00420755; written only for a ranked server
		field("minUnlockLevel", &S::minUnlockLevel), // 0x0041f0a0 0x0041f121 0x0041f16e 0x0041f1de
		field("maxUnlockLevel", &S::maxUnlockLevel), // 0x0041ef1d 0x0041ef9e 0x0041efeb 0x0041f05b
		field("sponsorText", &S::sponsorText, true), // 0x0041f9c9 0x0041fa46 0x0041fab5
		field("sponsorLogoURL", &S::sponsorLogoURL, true), // 0x0041fb2a 0x0041fba7 0x0041fc16
		field("communityLogoURL", &S::communityLogoURL, true), // 0x0041fc8b 0x0041fd08 0x0041fd14 0x0041fd83
		field("customMapURL", &S::customMapURL, true), // 0x0041fdf8 0x0041fe75 0x0041fee6
		field("radioSpamInterval", &S::radioSpamInterval), // 0x0041ff5b 0x0041ffcc 0x00420019
		field("radioMaxSpamFlagCount", &S::radioMaxSpamFlagCount), // 0x00420063 0x004200d4 0x00420121
		field("radioBlockedDurationTime", &S::radioBlockedDurationTime), // 0x0042016b 0x004201dc 0x00420229
		field("numReservedSlots", &S::numReservedSlots), // 0x0041c528 0x0041c5a9 0x0041c5f6 0x0041c666
		field("maxRank", &S::maxRank), // 0x0041c6ab 0x0041c72c 0x0041c779 0x0041c7ee
		field("friendlyFireWithMines", &S::friendlyFireWithMines), // 0x0041d7fe 0x0041d86f 0x0041d8bc
		field("botSkill", &S::botSkill), // 0x0042047f 0x004204f0 0x0042053d
		field("allowSpectators", &S::allowSpectators), // 0x0041c0ec 0x0041c16d 0x0041c1ba 0x0041c22f 0x0041c276
		field("allowTitanMovement", &S::allowTitanMovement), // 0x00420588 0x004205f9 0x00420646
		field("configFile", &S::configFile, false, false), // 0x0041b11d 0x0041b19a 0x0041b200
		field("maxConnectionType", &S::maxConnectionType, false, false), // 0x0041b823 0x0041b894 0x0041b8ec 0x0041b939
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

// The listing (0x00506b60 builds a line per ban: "IP: " / "Key: ", " Time
// Left: " or " Perm"; 0x00507800 / 0x005078d0 cut the address and key
// lists from it).
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
	// 0x00507ef0 (the period read by 0x00507ff0).
	return addBan(m_addresses, hostOf(address), period, now);
}

bool BanList::addKey(const std::string& key, const std::string& period, std::time_t now)
{
	// 0x00507f20 (the period read by 0x00508030).
	return addBan(m_keys, key, period, now);
}

void BanList::removeAddress(const std::string& address)
{
	// 0x005077b0
	const std::string host = hostOf(address);
	m_addresses.erase(std::remove_if(m_addresses.begin(), m_addresses.end(), [&](const Ban& b) { return b.what == host; }),
		m_addresses.end());
}

void BanList::removeKey(const std::string& key)
{
	// 0x005077d0
	m_keys.erase(std::remove_if(m_keys.begin(), m_keys.end(), [&](const Ban& b) { return b.what == key; }), m_keys.end());
}

void BanList::clear()
{
	// 0x005077f0
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
