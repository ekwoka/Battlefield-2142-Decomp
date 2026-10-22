#include "MenuBindings.h"

#include "EndOfRoundBindings.h"
#include "Input/ControlMap.h"
#include "Net/Fesl.h"
#include "Net/Socket.h"
#include "UI/Player/Natives.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>

namespace dice::hfe::menu {

namespace avm1 = ui::avm1;
using avm1::Interpreter;
using avm1::Value;
using Args = std::vector<Value>;

namespace {

constexpr const char* kLogic = "dice.bf2.Logic";
constexpr const char* kProfile = "dice.bf2.Profile";
constexpr const char* kMultiplay = "dice.bf2.Multiplay";
constexpr const char* kServerInfo = "dice.bf2.ServerInfo";
constexpr const char* kMessages = "dice.MessageHandler";
constexpr const char* kServerSettings = "dice.bf2.ServerSettings";
constexpr const char* kGeneralSettings = "dice.bf2.GeneralSettings";
constexpr const char* kAudioSettings = "dice.bf2.AudioSettings";
constexpr const char* kVideoSettings = "dice.bf2.VideoSettings";
constexpr const char* kControlSettings = "dice.bf2.ControlSettings";
constexpr const char* kSettingsObjects[] = {kServerSettings, kGeneralSettings, kAudioSettings, kVideoSettings, kControlSettings};

constexpr const char* kModVersion = "1.51";
// The client's version as servers report theirs (gamever).
constexpr const char* kGameVersion = "1.10.112.0";
constexpr const char* kModName = "bf2142";

// The server lists that start with the "Favorites" / "Live servers" rows.
constexpr int kHeaderRows = 3;

Value arg(const Args& args, size_t i) { return i < args.size() ? args[i] : Value(); }

bool isServerList(const std::string& id)
{
	return id == "serverListQuick" || id == "serverListAdvanced" || id == "serverList" || id == "serverListLAN"
		|| id == "serverHistory" || id == "favouriteServers";
}
bool hasHeaderRows(const std::string& id) { return id == "serverListQuick" || id == "serverListAdvanced"; }
// Lists filled from the master server's answer.
bool isInternetList(const std::string& id)
{
	return id == "serverListQuick" || id == "serverListAdvanced" || id == "serverList";
}

std::string field(const net::ServerEntry& server, const char* name)
{
	const auto it = server.fields.find(name);
	return it == server.fields.end() ? std::string() : it->second;
}
int fieldNumber(const net::ServerEntry& server, const char* name) { return std::atoi(field(server, name).c_str()); }

// "remember me": the password is kept scrambled, not in clear text.
std::string scramble(const std::string& text)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < text.size(); ++i) {
		const auto c = static_cast<unsigned char>(text[i] ^ (0x5a + i * 7));
		out += digits[c >> 4];
		out += digits[c & 15];
	}
	return out;
}
// What the password field shows for a remembered login: 12 characters
// standing for it.
std::string tokenOf(const std::string& name, const std::string& remembered)
{
	uint64_t h = 1469598103934665603ull;
	for (char c : name + "\n" + remembered) {
		h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
	}
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (int i = 0; i < 12; ++i) {
		out += digits[(h >> (i * 4)) & 15];
	}
	return out;
}

std::string unscramble(const std::string& hex)
{
	std::string out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		const auto c = static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
		out += static_cast<char>(c ^ (0x5a + (i / 2) * 7));
	}
	return out;
}

} // namespace

// --- Errors

int profileErrorFromFesl(long code)
{
	// 0x004a1730: FESL's errorCode -> the ProfileManager's error.
	switch (code) {
	case 0: return kProfileErrorNone;
	case 20: return 37;  // user not authenticated
	case 21: return 5;   // invalid fields
	case -0xa5: return 6;
	case -0x68: return 1;
	case -0x66: return kProfileErrorTimeout;
	case 99: return kProfileErrorSystem;
	case 101: return kProfileErrorAccountNotFound;
	case 102: return kProfileErrorAccountDisabled;
	case 103: return kProfileErrorAccountBanned;
	case 105: return 22; // not confirmed
	case 121: return 31; // too many login attempts
	case 122: return kProfileErrorPasswordWrong;
	case 123: return 33; // game not registered
	case 140: return 30;
	case 141: return 29;
	case 163: return 32; // no more soldiers
	case 180: return 36;
	case 181:
	case 3004: return 35;
	case 182: return kProfileErrorNone;
	default: return kProfileErrorUnhandled;
	}
}

const char* profileErrorKey(int error)
{
	// 0x004a0980's table.
	static const char* const keys[] = {"", "", "LOCERROR_noconnection", "LOCERROR_servertimeout",
		"LOCERROR_genericloginerror", "LOCERROR_invalidparameter", "LOCERROR_underage", "LOCERROR_parentalemail",
		"LOCERROR_passwordwrong", "LOCERROR_passwordsnotidentical", "LOCERROR_passwordtooshort",
		"LOCERROR_passwordtoolong", "LOCERROR_passwordrequired", "LOCERROR_passwordinvalid",
		"LOCERROR_accountnameexists", "LOCERROR_accountnotfound", "LOCERROR_accountnametooshort",
		"LOCERROR_accountnametoolong", "LOCERROR_accountnamerequired", "LOCERROR_accountnameinvalid",
		"LOCERROR_accountdisabled", "LOCERROR_accountbanned", "LOCERROR_accountnotconfirmed",
		"LOCERROR_emailnotfound", "LOCERROR_emailrequired", "LOCERROR_emailinvalid", "LOCERROR_birthdaterequired",
		"LOCERROR_birthdateinvalid", "LOCERROR_countryrequired", "LOCERROR_toomanynamerecoveries",
		"LOCERROR_toomanypasswordrecoveries", "LOCERROR_toomanyloginattempts", "TEXT_youcantcreateanymoresoldiers",
		"LOCERROR_gamenotregistered", "LOCERROR_soldiernameexists", "LOCERROR_cdkeyinvalid",
		"LOCERROR_cdkeyalreadyused", "LOCERROR_usernotauthenticated", "LOCERROR_systemerror", "LOCERROR_unhandled"};
	return error >= 0 && error < static_cast<int>(sizeof(keys) / sizeof(keys[0])) ? keys[error] : "LOCERROR_unhandled";
}

// --- Background jobs

// One blocking call on its own thread; the menu polls `done`. The thread
// holds the job, so a job nobody waits for any more (cancelled, or the
// bindings gone) finishes on its own.
struct MenuBindings::Job {
	std::atomic<bool> done {false};
	AccountReply account;
	std::optional<net::ServerList> servers;
	net::ConnectResult connect;
	bool flag = false;
	std::string error;

	static std::shared_ptr<Job> start(std::function<void(Job&)> work)
	{
		auto job = std::make_shared<Job>();
		std::thread([job, work = std::move(work)]() {
			work(*job);
			job->done = true;
		}).detach();
		return job;
	}
	static bool running(const std::shared_ptr<Job>& job) { return job && !job->done; }
};

// --- OpenSpy

MenuServices MenuServices::openSpy()
{
	// One account session at a time, kept between the account and the
	// soldier login.
	struct Session {
		std::mutex mutex;
		std::unique_ptr<net::FeslClient> client;
	};
	auto session = std::make_shared<Session>();
	const auto failed = [](const std::optional<net::FeslMessage>& reply) {
		AccountReply out;
		if (!reply) {
			out.reached = false;
		} else {
			out.error = reply->number("errorCode", 99);
		}
		return out;
	};

	MenuServices services;
	services.checkOnline = []() {
		net::TcpSocket socket;
		return socket.connect(net::kFeslHost, net::kFeslPort, 5);
	};
	services.loginAccount = [session, failed](const std::string& name, const std::string& password) {
		std::lock_guard<std::mutex> lock(session->mutex);
		session->client = std::make_unique<net::FeslClient>();
		net::FeslClient& client = *session->client;
		AccountReply out;
		if (!client.connect(net::kFeslHost, net::kFeslPort, 10)) {
			out.reached = false;
			return out;
		}
		auto reply = client.hello();
		if (!reply || reply->isError()) {
			return failed(reply);
		}
		reply = client.login(name, password);
		if (!reply || reply->isError()) {
			return failed(reply);
		}
		reply = client.subAccounts();
		if (!reply || reply->isError()) {
			return failed(reply);
		}
		out.soldiers = reply->list("subAccounts");
		return out;
	};
	services.loginSoldier = [session, failed](const std::string& soldier) {
		std::lock_guard<std::mutex> lock(session->mutex);
		AccountReply out;
		if (!session->client || !session->client->connected()) {
			out.reached = false;
			return out;
		}
		auto reply = session->client->loginSubAccount(soldier);
		if (!reply || reply->isError()) {
			return failed(reply);
		}
		out.profileId = reply->number("profileId");
		session->client->gameSpyPreAuth(); // the ticket for GameSpy's services, not needed to play
		return out;
	};
	services.logoutAccount = [session]() {
		std::lock_guard<std::mutex> lock(session->mutex);
		if (session->client && session->client->connected()) {
			session->client->goodbye();
		}
		session->client.reset();
	};
	services.fetchServers = [](std::string* error) {
		// The fields the game's browser asks for.
		const std::vector<std::string> fields {"hostname", "gamever", "mapname", "gametype", "gamevariant", "numplayers",
			"maxplayers", "password", "hostport", "bf2142_pure", "bf2142_ranked", "bf2142_ranked_tournament",
			"bf2142_reservedslots", "bf2142_sponsortext", "bf2142_communitylogo_url", "bf2142_sponsorlogo_url",
			"bf2142_maxrank", "bf2142_pb"};
		return net::fetchServerList(net::masterServerHost(net::kGameName), "", fields, 15, error);
	};
	services.connectServer = [](const std::string& host, uint16_t port) {
		net::GameConnection connection;
		net::ConnectResult result = connection.connect(host, port, {}, 6000);
		if (result.outcome == net::ConnectResult::Outcome::Accepted) {
			connection.disconnect();
		}
		return result;
	};
	return services;
}

// --- The bindings

MenuBindings::MenuBindings(ui::NativeRegistry& natives, MenuServices services)
	: m_natives(natives), m_services(std::move(services))
{
	bindLogic();
	bindProfile();
	bindMessages();
	bindLists();
	bindMultiplay();
	bindServerSettings();
	bindControls();
}

MenuBindings::~MenuBindings() = default;

void MenuBindings::log(const std::string& line) const
{
	if (onLog) {
		onLog(line);
	}
}

bool MenuBindings::online() const { return m_online && !m_offline; }

void MenuBindings::finishJobs()
{
	for (int i = 0; i < 2000; ++i) {
		if (!Job::running(m_onlineCheck) && !Job::running(m_transaction) && !Job::running(m_query)
			&& !Job::running(m_joinProbe)) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

std::string MenuBindings::joinOutcome() const
{
	if (!m_joinProbe || !m_joinProbe->done) {
		return {};
	}
	const net::ConnectResult& r = m_joinProbe->connect;
	switch (r.outcome) {
	case net::ConnectResult::Outcome::Accepted: return "accepted";
	case net::ConnectResult::Outcome::Refused: return std::string("refused: ") + net::refusalReason(r.reason);
	case net::ConnectResult::Outcome::NoAnswer: return "no answer";
	case net::ConnectResult::Outcome::Error: return "error: " + r.message;
	}
	return {};
}

std::string MenuBindings::loginToken(const LocalAccount& local) { return tokenOf(local.name, local.remembered); }

// The handshake probe's outcome: a refusal or silence shows as the game's
// LOCERROR_ message (type 3 carries the key without the prefix).
void MenuBindings::pollJoin()
{
	if (!m_joinProbe || !m_joinProbe->done || m_joinReported) {
		return;
	}
	m_joinReported = true;
	const net::ConnectResult& r = m_joinProbe->connect;
	log(m_joinHost + ":" + std::to_string(m_joinPort) + ": " + joinOutcome());
	if (r.outcome == net::ConnectResult::Outcome::Accepted) {
		return;
	}
	const char* key = "connectionfailed";
	if (r.outcome == net::ConnectResult::Outcome::Refused) {
		switch (r.reason) {
		case 0x11: key = "invalidsrvpassword"; break;
		case 0x16: key = "banned"; break;
		case 0x1e: key = "serverfull"; break;
		case 0x1f: key = "srvpunkbuster"; break;
		default: break;
		}
	}
	raiseMessage(3, key, static_cast<int>(r.reason));
}

void MenuBindings::raiseMessage(int type, const std::string& text, int code)
{
	m_messages.push_back({type, code, text});
}

// --- dice.bf2.Logic (0x1003e0c0)

void MenuBindings::bindLogic()
{
	ui::NativeRegistry& n = m_natives;
	n.setFunction(kLogic, "quit", [this](Interpreter&, const Args&) {
		if (onQuit) {
			onQuit();
		}
		return Value();
	});
	n.setFunction(kLogic, "getCommandline", [](Interpreter&, const Args&) { return Value(""); });
	n.setFunction(kLogic, "getModVersion", [](Interpreter&, const Args&) { return Value(kModVersion); });
	// No patch server to ask: the check is over at once and finds nothing.
	n.setFunction(kLogic, "checkVersion", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "getVersionResult", [](Interpreter&, const Args&) { return Value(0); });
	n.setFunction(kLogic, "getEADownloader", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "isGameRunning", [this](Interpreter&, const Args&) { return Value(m_gameRunning); });
	n.setFunction(kLogic, "disconnectGame", [this](Interpreter&, const Args&) {
		log("disconnect");
		if (onDisconnect) {
			onDisconnect();
		}
		return Value();
	});
	// 0x1003dc70: offline mode for an account's soldiers kept here, or (no
	// name) back to online. An account never seen gets one soldier of its
	// own name, so that the menu can be used without the account service
	// (the game asked EA Online whether the account exists).
	n.setFunction(kLogic, "setOfflineMode", [this](Interpreter& vm, const Args& args) {
		const std::string name = args.empty() ? std::string() : vm.toString(args[0]);
		if (name.empty()) {
			m_offline = false;
			return Value(false);
		}
		LocalAccount* local = localAccount(name);
		if (!local) {
			m_local.push_back({name, {name}, {}});
			local = &m_local.back();
			saveProfiles();
		}
		m_offline = true;
		m_account = {};
		m_account.name = local->name;
		m_account.soldiers = local->soldiers;
		m_account.loggedIn = true;
		setSoldierRows(m_account.soldiers);
		log("offline mode: account " + name);
		return Value(true);
	});
}

// --- dice.bf2.Profile (0x100516b0)

// A soldier's standing (rank, experience, unlocks): the stats server's
// answers, here from the soldier's record in the local store (Stats.h).
// Without a soldier the numbers are a fresh recruit's. Awards and veteran
// status are not kept.
void MenuBindings::bindProfileStats()
{
	ui::NativeRegistry& n = m_natives;
	const auto constant = [&n](const char* member, Value value) {
		n.setFunction(kProfile, member, [value](Interpreter&, const Args&) { return value; });
	};
	// The soldier's numbers (a recruit's without one).
	const auto number = [this, &n](const char* member, std::function<double(const stats::Soldier&)> get) {
		n.setFunction(kProfile, member, [this, get](Interpreter&, const Args&) {
			stats::SoldierStats recruit;
			stats::SoldierStats* record = soldierStats();
			return Value(get(stats::Soldier(record ? *record : recruit, stats::ranks(), m_booster)));
		});
	};
	constant("isVeteran", Value(false));
	constant("getLatestAwards", Value(""));
	constant("showVeteranPopup", Value(false));
	constant("showRetailerRankupPopup", Value(false));
	constant("showRetailerWeaponPopup", Value(false));
	// 0x1004a920: always; 0x1004a8e0 (0x1004a7f0): offline, or the
	// account's booster flag. The unlock pages then show Northern Strike's.
	constant("hasBoosterPatch", Value(true));
	n.setFunction(kProfile, "hasBoosterPack", [this](Interpreter&, const Args&) { return Value(m_booster); });
	// 0x1004c940 / 0x10049950 / 0x10049990 / 0x100498b0 (0..1).
	number("getCurrentRankId", [](const stats::Soldier& s) { return s.rank(); });
	number("getCurrentExp", [this](const stats::Soldier&) {
		const stats::SoldierStats* record = soldierStats();
		return record ? record->experience : 0;
	});
	number("getNextRankExp", [](const stats::Soldier& s) { return s.nextRankPoints(); });
	number("getRankProgress", [](const stats::Soldier& s) { return s.rankProgress(); });
	// 0x1004c510: the ranks there are (RankSettings.con's 0..40).
	constant("getNumRanks", Value(stats::ranks().maxRank() + 1));
	// 0x1004df60, 0x10049a40 (the unspent credits), 0x1004b190 (40, 50
	// with the booster), 0x1004de60 (selected / 40 or 50, at most 1).
	number("getUnlocksCountSelected", [](const stats::Soldier& s) { return s.unlocksSelected(); });
	number("getUnlocksCountUnselected", [](const stats::Soldier& s) { return s.credits(); });
	number("getUnlocksCountTotal", [](const stats::Soldier& s) { return s.unlocksTotal(); });
	number("getUnlocksProgress", [](const stats::Soldier& s) {
		return std::min(1.0, static_cast<double>(s.unlocksSelected()) / s.unlocksTotal());
	});
	n.setFunction(kProfile, "getCurrentRankString", [this, &n](Interpreter&, const Args&) {
		stats::SoldierStats recruit;
		stats::SoldierStats* record = soldierStats();
		return Value(n.localize("RANK_" + std::to_string(stats::Soldier(record ? *record : recruit).rank())));
	});
	n.setFunction(kProfile, "getNextRankString", [this, &n](Interpreter&, const Args&) {
		stats::SoldierStats recruit;
		stats::SoldierStats* record = soldierStats();
		const int rank = stats::Soldier(record ? *record : recruit).rank();
		return Value(n.localize("RANK_" + std::to_string(std::min(rank + 1, stats::ranks().maxRank()))));
	});
	// 0x1004e000: getUnlockInfo(id) is 0 locked, 1 taken, 2 available.
	n.setFunction(kProfile, "getUnlockInfo", [this](Interpreter& vm, const Args& args) {
		stats::SoldierStats* record = soldierStats();
		if (!record) {
			return Value(-1); // no unlock info (0x1004e000 answers -1)
		}
		const int id = static_cast<int>(vm.toNumber(arg(args, 0)));
		return Value(static_cast<int>(stats::Soldier(*record, stats::ranks(), m_booster).status(id)));
	});
	// 0x1004b250: selectUnlock(id, newRequest) starts the server's
	// transaction (true) and asks for its outcome (false; -1 while it
	// runs). Here the store answers at once: 0 done, 1 not available, 302
	// invalid, 303 selected already, 304 no credits (the menu's
	// selectUnlockDone).
	n.setFunction(kProfile, "selectUnlock", [this](Interpreter& vm, const Args& args) {
		stats::SoldierStats* record = soldierStats();
		if (!record) {
			return Value(static_cast<int>(stats::kSelectUnavailable));
		}
		const int id = static_cast<int>(vm.toNumber(arg(args, 0)));
		const stats::SelectResult result = stats::Soldier(*record, stats::ranks(), m_booster).select(id);
		if (result == stats::kSelectDone) {
			m_stats.save();
			log("unlock " + std::to_string(id) + " for " + record->name);
		}
		return Value(static_cast<int>(result));
	});
	// The main menu's rank popup (checkRankNotification): 1 a new rank
	// since the last look, 2 a lower one (not here). Once.
	n.setFunction("dice.bf2.PlayerStats", "getRankNotification", [this](Interpreter&, const Args&) {
		const int notification = m_rankNotification;
		m_rankNotification = 0;
		return Value(notification);
	});
	n.setFunction("dice.bf2.PlayerStats", "clearRankNotification", [this](Interpreter&, const Args&) {
		m_rankNotification = 0;
		return Value();
	});
}

stats::SoldierStats* MenuBindings::soldierStats()
{
	return m_soldier.empty() || !m_account.loggedIn ? nullptr : &m_stats.soldier(m_soldier);
}

std::vector<int> MenuBindings::soldierUnlocks()
{
	const stats::SoldierStats* record = soldierStats();
	return record ? record->unlocks : std::vector<int>();
}

void MenuBindings::recordRound(RoundSummary& summary)
{
	stats::SoldierStats* record = soldierStats();
	if (!record || !summary.validRound || summary.localPlayer < 0) {
		return;
	}
	stats::RoundResult round;
	round.score = summary.score;
	round.kills = summary.kills;
	round.deaths = summary.deaths;
	round.seconds = summary.roundSeconds;
	round.outcome = summary.winningTeam == 0 ? 0 : summary.winningTeam == summary.myTeam ? 1 : -1;
	stats::Soldier soldier(*record, stats::ranks(), m_booster);
	summary.rankedUp = soldier.record(round);
	summary.roundExp = std::max(0, round.score);
	summary.totalExp = record->experience;
	summary.rank = soldier.rank();
	if (summary.rankedUp) {
		m_rankNotification = 1;
	}
	m_stats.save();
	log(record->name + ": " + std::to_string(summary.roundExp) + " points, " + std::to_string(record->experience)
		+ " in all, rank " + std::to_string(summary.rank) + (summary.rankedUp ? " (new)" : ""));
}

// The "stats" list (0x1007f640; its labels 0x1007dfd0): label, value.
// Only what the store keeps is listed: the overview's and the player's
// rows of it; the other categories (titan mode, teamwork, leadership,
// weapons, vehicles, maps) need stats not kept yet.
std::vector<std::vector<std::string>> MenuBindings::statsRows(int category)
{
	stats::SoldierStats recruit;
	const stats::SoldierStats* record = soldierStats();
	const stats::SoldierStats& s = record ? *record : recruit;
	const auto text = [](double value, int decimals) {
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
		return std::string(buffer);
	};
	const long long seconds = static_cast<long long>(s.timePlayed);
	char played[32];
	std::snprintf(played, sizeof(played), "%lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
	const double minutes = s.timePlayed / 60.0;
	const auto per = [&](double count, double over) { return over > 0 ? count / over : 0.0; };
	std::vector<std::pair<const char*, std::string>> rows;
	if (category == 0) {
		rows = {{"TEXT_careerpoints", std::to_string(s.experience)}, {"TEXT_globalscore", std::to_string(s.experience)},
			{"TEXT_timeplayed", played}, {"TEXT_winslosses", std::to_string(s.wins) + " / " + std::to_string(s.losses)},
			{"TEXT_bestroundscore", std::to_string(s.bestRoundScore)}};
	} else if (category == 1) {
		rows = {{"TEXT_scoreperminute", text(per(s.experience, minutes), 2)},
			{"TEXT_kdratio", text(per(s.kills, std::max(1, s.deaths)), 2)}, {"TEXT_kills", std::to_string(s.kills)},
			{"TEXT_deaths", std::to_string(s.deaths)}, {"TEXT_killsperminute", text(per(s.kills, minutes), 2)},
			{"TEXT_deathsperminute", text(per(s.deaths, minutes), 2)},
			{"TEXT_killsperround", text(per(s.kills, s.rounds), 2)}, {"TEXT_deathsperround", text(per(s.deaths, s.rounds), 2)}};
	}
	std::vector<std::vector<std::string>> out;
	for (const auto& [key, value] : rows) {
		out.push_back({m_natives.localize(key), value});
	}
	return out;
}

void MenuBindings::bindProfile()
{
	bindProfileStats();
	ui::NativeRegistry& n = m_natives;
	// 0x1004b430: (true) starts the check, (false) tells whether it is
	// over (the result is isOnline()).
	n.setFunction(kProfile, "checkOnlineStatus", [this](Interpreter&, const Args& args) {
		if (avm1::toBoolean(arg(args, 0))) {
			if (!Job::running(m_onlineCheck)) {
				const auto check = m_services.checkOnline;
				m_onlineCheck = Job::start([check](Job& job) { job.flag = check && check(); });
			}
			return Value(false);
		}
		if (Job::running(m_onlineCheck)) {
			return Value(false);
		}
		if (m_onlineCheck) {
			m_online = m_onlineCheck->flag;
			m_onlineCheck.reset();
			log(std::string("account service ") + (m_online ? "reachable" : "unreachable"));
		}
		return Value(true);
	});
	n.setFunction(kProfile, "isOnline", [this](Interpreter&, const Args&) { return Value(online()); });
	n.setFunction(kProfile, "isConnected", [this](Interpreter&, const Args&) { return Value(m_account.connected); });
	// 0x1004d7d0: 2 logged in to EA Online, 1 to a local profile, 0 none.
	n.setFunction(kProfile, "getLoginStatus", [this](Interpreter&, const Args&) {
		return Value(m_account.connected ? 2 : m_account.loggedIn ? 1 : 0);
	});
	n.setFunction(kProfile, "getLastOnlineUser", [this](Interpreter&, const Args&) { return Value(m_lastUser); });
	// The remembered login of the last user: the login page puts it in the
	// password field (at most 16 characters) and sends it back to
	// connectToMasterAccountEncrypted unless the fields were edited.
	n.setFunction(kProfile, "getEncryptedLogin", [this](Interpreter&, const Args&) {
		const LocalAccount* local = localAccount(m_lastUser);
		return Value(local && !local->remembered.empty() ? loginToken(*local) : std::string());
	});
	// 0x1004bc20: (name, password, remember).
	n.setFunction(kProfile, "connectToMasterAccount", [this](Interpreter& vm, const Args& args) {
		startAccountLogin(vm.toString(arg(args, 0)), vm.toString(arg(args, 1)), avm1::toBoolean(arg(args, 2)));
		return Value(true);
	});
	n.setFunction(kProfile, "connectToMasterAccountEncrypted", [this](Interpreter& vm, const Args& args) {
		const std::string token = vm.toString(arg(args, 0));
		const LocalAccount* local = localAccount(m_lastUser);
		if (local && !local->remembered.empty() && loginToken(*local) == token) {
			startAccountLogin(local->name, unscramble(local->remembered), avm1::toBoolean(arg(args, 1)));
			return Value(true);
		}
		startAccountLogin(m_lastUser, std::string(), false);
		return Value(true);
	});
	// 0x100510c0: a soldier of the account; offline at once, online through
	// the account service.
	n.setFunction(kProfile, "connectToSubAccount", [this](Interpreter& vm, const Args& args) {
		const std::string soldier = vm.toString(arg(args, 0));
		m_lastError = kProfileErrorNone;
		if (std::find(m_account.soldiers.begin(), m_account.soldiers.end(), soldier) == m_account.soldiers.end()) {
			m_transaction.reset();
			m_transactionKind = Transaction::None;
			m_lastError = kProfileErrorAccountNotFound;
			return Value(false);
		}
		m_transactionName = soldier;
		m_transactionKind = Transaction::Soldier;
		if (!m_account.connected) {
			m_transaction.reset();
			finishSoldierLogin();
			return Value(true);
		}
		const auto login = m_services.loginSoldier;
		m_transaction = Job::start([login, soldier](Job& job) {
			if (login) {
				job.account = login(soldier);
			} else {
				job.account.reached = false;
			}
		});
		return Value(true);
	});
	n.setFunction(kProfile, "disconnectFromSubAccount", [this](Interpreter&, const Args&) {
		log("soldier " + m_soldier + " logged out");
		m_soldier.clear();
		return Value();
	});
	// 0x10049bf0: whether the running transaction is over (and, when it
	// is, the results are taken over).
	n.setFunction(kProfile, "isTransactionComplete", [this](Interpreter&, const Args&) {
		if (Job::running(m_transaction)) {
			return Value(false);
		}
		if (m_transactionKind == Transaction::Account) {
			finishAccountLogin();
		} else if (m_transactionKind == Transaction::Soldier) {
			finishSoldierLogin();
		}
		return Value(true);
	});
	n.setFunction(kProfile, "cancelTransaction", [this](Interpreter&, const Args&) {
		m_transaction.reset();
		m_transactionKind = Transaction::None;
		return Value();
	});
	n.setFunction(kProfile, "getLastError", [this](Interpreter&, const Args&) { return Value(m_lastError); });
	n.setFunction(kProfile, "getLastErrorString", [this](Interpreter&, const Args&) {
		std::string text = m_natives.localize(profileErrorKey(m_lastError));
		const size_t at = text.find("#ERR#");
		if (at != std::string::npos) {
			text.replace(at, 5, std::to_string(m_lastFeslError));
		}
		return Value(text);
	});
	n.setFunction(kProfile, "logout", [this](Interpreter&, const Args&) {
		if (m_account.connected && m_services.logoutAccount) {
			const auto logout = m_services.logoutAccount;
			Job::start([logout](Job&) { logout(); });
		}
		log("account " + m_account.name + " logged out");
		m_account = {};
		m_soldier.clear();
		m_offline = false;
		m_natives.setListRows("soldiers", {});
		return Value();
	});
	n.setFunction(kProfile, "getNamePrefix", [this](Interpreter&, const Args&) { return Value(m_namePrefix); });
	n.setFunction(kProfile, "setNamePrefix", [this](Interpreter& vm, const Args& args) {
		m_namePrefix = arg(args, 0).isUndefined() ? std::string() : vm.toString(arg(args, 0));
		return Value();
	});
	n.setFunction(kProfile, "getActivePlayer", [this](Interpreter&, const Args&) { return Value(m_soldier); });

	// The offline profiles of the old login page (frame "offline"): local
	// accounts by index.
	n.setFunction(kProfile, "createOfflineAccount", [this](Interpreter& vm, const Args& args) {
		const std::string name = vm.toString(arg(args, 0));
		if (name.empty() || localAccount(name)) {
			return Value(false);
		}
		m_local.push_back({name, {name}, {}});
		saveProfiles();
		return Value(true);
	});
	n.setFunction(kProfile, "loginOfflineAccount", [this](Interpreter& vm, const Args& args) {
		const int index = static_cast<int>(vm.toNumber(arg(args, 0)));
		if (index < 0 || index >= static_cast<int>(m_local.size())) {
			return Value(false);
		}
		m_offline = true;
		m_account = {};
		m_account.name = m_local[static_cast<size_t>(index)].name;
		m_account.soldiers = m_local[static_cast<size_t>(index)].soldiers;
		m_account.loggedIn = true;
		m_soldier = m_account.soldiers.empty() ? m_account.name : m_account.soldiers.front();
		return Value(true);
	});
	n.setFunction(kProfile, "getNumLocalProfiles", [this](Interpreter&, const Args&) {
		return Value(static_cast<double>(m_local.size()));
	});
}

void MenuBindings::startAccountLogin(const std::string& name, const std::string& password, bool remember)
{
	m_lastError = kProfileErrorNone;
	m_transactionKind = Transaction::Account;
	m_transactionName = name;
	m_transactionPassword = password;
	m_transactionRemember = remember;
	const auto login = m_services.loginAccount;
	m_transaction = Job::start([login, name, password](Job& job) {
		if (login) {
			job.account = login(name, password);
		} else {
			job.account.reached = false;
		}
	});
	log("logging in to account " + name);
}

void MenuBindings::finishAccountLogin()
{
	m_transactionKind = Transaction::None;
	if (!m_transaction) {
		return;
	}
	const AccountReply reply = m_transaction->account;
	m_transaction.reset();
	m_lastFeslError = reply.error;
	m_lastError = reply.reached ? profileErrorFromFesl(reply.error) : kProfileErrorNoConnection;
	if (m_lastError != kProfileErrorNone) {
		log("login to " + m_transactionName + " failed: " + profileErrorKey(m_lastError) + " (" + std::to_string(reply.error) + ")");
		return;
	}
	m_offline = false;
	m_online = true;
	m_account = {};
	m_account.name = m_transactionName;
	m_account.soldiers = reply.soldiers;
	m_account.loggedIn = true;
	m_account.connected = true;
	setSoldierRows(m_account.soldiers);
	// Kept for offline mode and the next start.
	LocalAccount* local = localAccount(m_account.name);
	if (!local) {
		m_local.push_back({m_account.name, {}, {}});
		local = &m_local.back();
	}
	local->soldiers = reply.soldiers;
	local->remembered = m_transactionRemember ? scramble(m_transactionPassword) : std::string();
	m_lastUser = m_account.name;
	saveProfiles();
	std::string soldiers;
	for (const std::string& s : reply.soldiers) {
		soldiers += " " + s;
	}
	log("logged in to account " + m_account.name + "; soldiers:" + soldiers);
}

void MenuBindings::finishSoldierLogin()
{
	m_transactionKind = Transaction::None;
	if (m_transaction) {
		const AccountReply reply = m_transaction->account;
		m_transaction.reset();
		m_lastFeslError = reply.error;
		m_lastError = reply.reached ? profileErrorFromFesl(reply.error) : kProfileErrorNoConnection;
		if (m_lastError != kProfileErrorNone) {
			log("soldier " + m_transactionName + " failed: " + profileErrorKey(m_lastError));
			return;
		}
	}
	m_soldier = m_transactionName;
	log("playing as " + m_soldier + (m_account.connected ? "" : " (offline)"));
}

void MenuBindings::setSoldierRows(const std::vector<std::string>& soldiers)
{
	std::vector<std::vector<std::string>> rows;
	for (const std::string& soldier : soldiers) {
		rows.push_back({soldier});
	}
	m_natives.setListRows("soldiers", rows);
}

MenuBindings::LocalAccount* MenuBindings::localAccount(const std::string& name)
{
	for (LocalAccount& local : m_local) {
		if (local.name == name) {
			return &local;
		}
	}
	return nullptr;
}

// --- Local profiles: "last <name>", "account <name> <scrambled password or ->"
// and "soldier <account> <soldier>" lines.

std::string MenuBindings::defaultProfileFile()
{
	std::filesystem::path dir;
#if defined(_WIN32)
	if (const char* appData = std::getenv("APPDATA")) {
		dir = appData;
	}
#elif defined(__APPLE__)
	if (const char* home = std::getenv("HOME")) {
		dir = std::filesystem::path(home) / "Library" / "Application Support";
	}
#else
	if (const char* config = std::getenv("XDG_CONFIG_HOME")) {
		dir = config;
	} else if (const char* home = std::getenv("HOME")) {
		dir = std::filesystem::path(home) / ".config";
	}
#endif
	return dir.empty() ? std::string() : (dir / "bf2142-recomp" / "profiles.txt").string();
}

void MenuBindings::setProfileFile(const std::string& path)
{
	m_profileFile = path;
	loadProfiles();
}

void MenuBindings::loadProfiles()
{
	std::ifstream in(m_profileFile);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string kind, name, value;
		words >> kind >> name >> value;
		if (kind == "last") {
			m_lastUser = name;
		} else if (kind == "account" && !name.empty() && !localAccount(name)) {
			m_local.push_back({name, {}, value == "-" ? std::string() : value});
		} else if (kind == "soldier" && !value.empty()) {
			if (LocalAccount* local = localAccount(name)) {
				local->soldiers.push_back(value);
			}
		}
	}
	std::vector<std::vector<std::string>> rows;
	for (const LocalAccount& local : m_local) {
		rows.push_back({local.name});
	}
	m_natives.setListRows("localProfiles", rows);
}

void MenuBindings::saveProfiles()
{
	std::vector<std::vector<std::string>> rows;
	for (const LocalAccount& local : m_local) {
		rows.push_back({local.name});
	}
	m_natives.setListRows("localProfiles", rows);
	if (m_profileFile.empty()) {
		return;
	}
	std::error_code ignored;
	std::filesystem::create_directories(std::filesystem::path(m_profileFile).parent_path(), ignored);
	std::ofstream out(m_profileFile);
	if (!m_lastUser.empty()) {
		out << "last " << m_lastUser << "\n";
	}
	for (const LocalAccount& local : m_local) {
		out << "account " << local.name << " " << (local.remembered.empty() ? "-" : local.remembered) << "\n";
		for (const std::string& soldier : local.soldiers) {
			out << "soldier " << local.name << " " << soldier << "\n";
		}
	}
}

// --- dice.MessageHandler (0x1003f7d0): the newest message first.

void MenuBindings::bindMessages()
{
	ui::NativeRegistry& n = m_natives;
	// 0x1003f430: (text, type, code).
	n.setFunction(kMessages, "raiseMessage", [this](Interpreter& vm, const Args& args) {
		raiseMessage(static_cast<int>(vm.toNumber(arg(args, 1))), vm.toString(arg(args, 0)),
			static_cast<int>(vm.toNumber(arg(args, 2))));
		return Value();
	});
	// The menu asks every frame (checkForMessages): a join that failed
	// shows here.
	n.setFunction(kMessages, "getNumMessages", [this](Interpreter&, const Args&) {
		pollJoin();
		return Value(static_cast<double>(m_messages.size()));
	});
	n.setFunction(kMessages, "getLastText", [this](Interpreter&, const Args&) {
		return Value(m_messages.empty() ? std::string() : m_messages.back().text);
	});
	n.setFunction(kMessages, "getLastType", [this](Interpreter&, const Args&) {
		return Value(m_messages.empty() ? 0 : m_messages.back().type);
	});
	n.setFunction(kMessages, "getLastCode", [this](Interpreter&, const Args&) {
		return Value(m_messages.empty() ? 0 : m_messages.back().code);
	});
	n.setFunction(kMessages, "removeLast", [this](Interpreter&, const Args&) {
		if (!m_messages.empty()) {
			m_messages.pop_back();
		}
		return Value();
	});
	n.setFunction(kMessages, "clear", [this](Interpreter&, const Args&) {
		m_messages.clear();
		return Value();
	});
	n.setFunction(kMessages, "enableMessageHandler", [](Interpreter&, const Args&) { return Value(); });
}

// --- dice.bf2.List: the server lists from the master server, the soldiers
// and local profiles from the account. Every list's methods come here (the
// registry replaces a method for all lists), so the others get the
// player's usual behaviour.

void MenuBindings::startServerQuery()
{
	if (Job::running(m_query)) {
		return;
	}
	const auto fetch = m_services.fetchServers;
	m_query = Job::start([fetch](Job& job) {
		if (fetch) {
			job.servers = fetch(&job.error);
		} else {
			job.error = "no master server";
		}
	});
	log("querying the master server");
}

// Takes a finished query's servers; true while one still runs.
bool MenuBindings::finishServerQuery()
{
	if (Job::running(m_query)) {
		return true;
	}
	if (m_query) {
		m_servers.clear();
		if (m_query->servers) {
			for (const net::ServerEntry& entry : m_query->servers->servers) {
				Server server;
				server.entry = entry;
				const int port = fieldNumber(entry, "hostport");
				server.gamePort = static_cast<uint16_t>(port > 0 ? port : net::kGamePort);
				m_servers.push_back(std::move(server));
			}
			log(std::to_string(m_servers.size()) + " servers listed");
		} else {
			log("server list failed: " + m_query->error);
		}
		m_query.reset();
		showServers();
	}
	return false;
}

void MenuBindings::showServers()
{
	std::vector<std::vector<std::string>> rows;
	for (const Server& server : m_servers) {
		const net::ServerEntry& e = server.entry;
		std::string map = field(e, "mapname");
		std::replace(map.begin(), map.end(), '_', ' ');
		const std::string type = field(e, "gametype");
		rows.push_back({field(e, "hostname"),
			std::to_string(fieldNumber(e, "numplayers")) + "/" + std::to_string(fieldNumber(e, "maxplayers")) + " (0)", "",
			map, type.empty() ? std::string() : m_natives.localize("TEXT_" + type)});
	}
	std::vector<std::vector<std::string>> withHeaders {
		{m_natives.localize("SERVERBROWSER_favorites")}, {}, {m_natives.localize("SERVERBROWSER_liveservers")}};
	withHeaders.insert(withHeaders.end(), rows.begin(), rows.end());
	m_natives.setListRows("serverListQuick", withHeaders);
	m_natives.setListRows("serverListAdvanced", withHeaders);
	m_natives.setListRows("serverList", rows);
}

void MenuBindings::bindLists()
{
	ui::NativeRegistry& n = m_natives;
	using List = ui::NativeList;
	// The rows shown. On the server lists (0x10079770) the header rows are
	// not clickable and the ping bar (cell2, a clip: frame ping / 10, 20
	// for none) shows on server rows only.
	const auto show = [this](Interpreter& vm, List& list) {
		const int count = static_cast<int>(list.rows().size());
		const int shown = std::max(1, list.visibleRows(vm));
		list.firstIndex = std::clamp(list.firstIndex, 0, std::max(0, count - shown));
		list.populate(vm);
		if (!isServerList(list.listId) || !list.holder) {
			return;
		}
		const std::string prefix = list.rowPrefix(vm);
		const int headers = hasHeaderRows(list.listId) ? kHeaderRows : 0;
		for (int i = 0; i < list.visibleRows(vm); ++i) {
			const Value row = vm.getMember(Value(list.holder), prefix + std::to_string(i));
			const int index = list.firstIndex + i;
			const bool server = index >= headers && index < count && isInternetList(list.listId)
				&& index - headers < static_cast<int>(m_servers.size());
			if (index < headers) {
				vm.setMember(row, "isClickable", Value(0));
			}
			const net::ServerEntry* e = server ? &m_servers[static_cast<size_t>(index - headers)].entry : nullptr;
			const auto visible = [&](const char* name, bool on) {
				const Value clip = vm.getMember(row, name);
				if (clip.isObject()) {
					vm.setMember(clip, "_visible", Value(on));
				}
			};
			visible("cell2", server);
			if (server) {
				vm.callMethod(vm.getMember(row, "cell2").object(), "gotoAndStop", {Value(20)});
			}
			const std::string mod = e ? field(*e, "gamevariant") : std::string();
			visible("mcIconFavAdd", server);
			visible("mcIconFavRemove", false);
			visible("mcIconInfo", server);
			visible("mcIconMod", server && !mod.empty() && mod != kModName);
			visible("mcIconPunkbuster", e && fieldNumber(*e, "bf2142_pb") != 0);
			visible("mcIconVoIP", false);
			visible("mcIconRankedServer", e && fieldNumber(*e, "bf2142_ranked") != 0);
			visible("mcIconRankRestricted", e && fieldNumber(*e, "bf2142_maxrank") != 0);
			visible("mcIconLocked", e && fieldNumber(*e, "password") != 0);
			visible("mcIconTv", false);
			visible("mcIconTvRed", false);
			visible("mcIconTournament", e && fieldNumber(*e, "bf2142_ranked_tournament") != 0);
		}
	};
	// 0x100398c0 / 0x100374c0: (true) starts filling, (false) asks whether
	// it still runs.
	const auto fill = [this, show](Interpreter& vm, List& list, const Args& args) {
		const bool start = avm1::toBoolean(arg(args, 0));
		if (start) {
			list.selectedIndex = -1;
		}
		if (isInternetList(list.listId)) {
			if (start) {
				startServerQuery();
			}
			const bool busy = finishServerQuery();
			show(vm, list);
			return Value(busy);
		}
		// The BFHQ details page: its category list ("statsTypes", 0x10080cb0)
		// and the category's rows ("stats" with "<category> 21").
		if (list.listId.compare(0, 9, "selectBox") == 0 && !list.args.empty() && list.args[0] == "statsTypes") {
			m_natives.setListRows("statsTypes", ui::NativeRegistry::Rows {{m_natives.localize("TEXT_overview")}, {m_natives.localize("TEXT_player")},
					{m_natives.localize("TEXT_titanmode")}, {m_natives.localize("TEXT_teamwork")},
					{m_natives.localize("TEXT_leadership")}, {m_natives.localize("TEXT_weapons")},
					{m_natives.localize("TEXT_vehicles")}, {m_natives.localize("TEXT_maps")}});
		} else if (list.listId == "stats") {
			m_natives.setListRows(list.listId, statsRows(list.args.empty() ? 0 : std::atoi(list.args[0].c_str())));
		}
		show(vm, list);
		return Value(false); // the others are filled at once
	};
	n.setListMethod("fillList", fill);
	n.setListMethod("refreshList", fill);
	// 0x10038f00: selects a shown row and returns what it holds.
	n.setListMethod("selectRow", [this, show](Interpreter& vm, List& list, const Args& args) {
		const int row = static_cast<int>(vm.toNumber(arg(args, 0)));
		list.selectedIndex = row < 0 ? -1 : list.firstIndex + row;
		show(vm, list);
		const int index = list.selectedIndex;
		if (index < 0 || index >= static_cast<int>(list.rows().size())) {
			return Value();
		}
		if (list.listId == "mapListAvailable" || list.listId == "mapListChosen") {
			const auto maps = list.listId == "mapListChosen" ? m_mapList : availableMaps();
			auto object = vm.newObject();
			const MenuMap& map = maps[static_cast<size_t>(index)];
			vm.setMember(Value(object), "rowIndex", Value(index));
			vm.setMember(Value(object), "rowData", Value(map.level + " " + std::to_string(map.size)));
			return Value(object);
		}
		// The select boxes' rows: their index and text (the drop list's
		// rowRelease calls setValue(rowIndex, rowValue)).
		if (list.listId.compare(0, 9, "selectBox") == 0) {
			auto object = vm.newObject();
			vm.setMember(Value(object), "rowIndex", Value(index));
			vm.setMember(Value(object), "rowValue", Value(list.rows()[static_cast<size_t>(index)].at(0)));
			return Value(object);
		}
		if (list.listId == "soldiers") {
			auto object = vm.newObject();
			vm.setMember(Value(object), "soldierName", Value(list.rows()[static_cast<size_t>(index)].at(0)));
			return Value(object);
		}
		if (!isInternetList(list.listId)) {
			return Value();
		}
		const int server = index - (hasHeaderRows(list.listId) ? kHeaderRows : 0);
		if (server < 0 || server >= static_cast<int>(m_servers.size())) {
			return Value();
		}
		const Server& s = m_servers[static_cast<size_t>(server)];
		const net::ServerEntry& e = s.entry;
		// 0x100743xx: the row object.
		auto object = vm.newObject();
		const auto set = [&](const char* name, const Value& value) { vm.setMember(Value(object), name, value); };
		set("serverAddress", Value(e.ip + " " + std::to_string(s.gamePort) + " " + std::to_string(e.port)));
		set("password", Value(fieldNumber(e, "password") != 0 ? 1 : 0));
		const std::string mod = field(e, "gamevariant");
		set("sameMod", Value(mod.empty() || mod == kModName ? 1 : 0));
		const std::string version = field(e, "gamever");
		const int compare = version.empty() ? 0 : std::string(kGameVersion).compare(version);
		set("version", Value(compare < 0 ? -1 : compare > 0 ? 1 : 0));
		set("punkbuster", Value(fieldNumber(e, "bf2142_pb") != 0 ? 1 : 0));
		set("maxRank", Value(fieldNumber(e, "bf2142_maxrank") != 0 ? 1 : 0));
		set("hasReservedSlots", Value(fieldNumber(e, "bf2142_reservedslots") > 0 ? 1 : 0));
		set("isBoosterPackMap", Value(0));
		set("isCustomPackMap", Value(0));
		set("customMapLink", Value(""));
		return Value(object);
	});
	// The server info popup's lists get "[team] ip gameport queryport".
	n.setListMethod("setArgs", [this](Interpreter& vm, List& list, const Args& args) {
		list.args.clear();
		std::istringstream words(vm.toString(arg(args, 0)));
		for (std::string word; words >> word;) {
			list.args.push_back(word);
		}
		if (list.listId == "players" && list.args.size() >= 2) {
			m_infoHost = list.args[1];
		} else if (list.listId == "serverExtendedInfo" && !list.args.empty()) {
			m_infoHost = list.args[0];
		}
		return Value();
	});
}

// --- dice.bf2.Multiplay (0x100475c0)

void MenuBindings::bindMultiplay()
{
	ui::NativeRegistry& n = m_natives;
	// 0x10043ed0: "ip gameport queryport".
	n.setFunction(kMultiplay, "joinServer", [this](Interpreter& vm, const Args& args) {
		std::istringstream words(vm.toString(arg(args, 0)));
		std::string host;
		int port = 0;
		words >> host >> port;
		if (host.empty() || port <= 0 || port > 0xffff) {
			return Value(false);
		}
		m_joinHost = host;
		m_joinPort = static_cast<uint16_t>(port);
		// The map the browser listed for it (the level to load).
		m_joinMap = {};
		for (const Server& server : m_servers) {
			if (server.entry.ip == host && server.gamePort == m_joinPort) {
				m_joinMap = {field(server.entry, "mapname"), field(server.entry, "gametype"), fieldNumber(server.entry, "mapsize")};
			}
		}
		log("joining " + host + ":" + std::to_string(port));
		if (onJoin) {
			onJoin(m_joinHost, m_joinPort);
		}
		if (m_services.connectServer) {
			const auto connect = m_services.connectServer;
			const std::string h = m_joinHost;
			const uint16_t p = m_joinPort;
			m_joinReported = false;
			m_joinProbe = Job::start([connect, h, p](Job& job) { job.connect = connect(h, p); });
		}
		return Value(true);
	});
	n.setFunction(kMultiplay, "isGameSpyAvailable", [this](Interpreter&, const Args&) { return Value(m_account.connected); });
	n.setFunction(kMultiplay, "getNumServers", [this](Interpreter&, const Args&) {
		return Value(static_cast<double>(m_servers.size()));
	});
	n.setFunction(kMultiplay, "isBrowsing", [this](Interpreter&, const Args&) { return Value(Job::running(m_query)); });
	n.setFunction(kMultiplay, "isServerBrowserBusy", [this](Interpreter&, const Args&) { return Value(Job::running(m_query)); });
	n.setFunction(kMultiplay, "startBrowse", [this](Interpreter&, const Args&) {
		startServerQuery();
		return Value(true);
	});
	n.setFunction(kMultiplay, "refreshBrowser", [this](Interpreter&, const Args&) {
		startServerQuery();
		return Value(true);
	});
	n.setFunction(kMultiplay, "stopBrowser", [](Interpreter&, const Args&) { return Value(); });
	// --- Starting a game (the singleplayer page).
	n.setFunction(kMultiplay, "setFilterGameMode", [this](Interpreter& vm, const Args& args) {
		m_filterGameMode = vm.toString(arg(args, 0));
		showMapLists();
		return Value();
	});
	n.setFunction(kMultiplay, "getFilterGameMode", [this](Interpreter&, const Args&) { return Value(m_filterGameMode); });
	n.setFunction(kMultiplay, "clearMapList", [this](Interpreter&, const Args&) {
		m_mapList.clear();
		showMapLists();
		return Value();
	});
	// An index into mapListAvailable.
	n.setFunction(kMultiplay, "addMapIndexToMapList", [this](Interpreter& vm, const Args& args) {
		const auto maps = availableMaps();
		const double index = vm.toNumber(arg(args, 0));
		if (index >= 0 && index < static_cast<double>(maps.size())) {
			m_mapList.push_back(maps[static_cast<size_t>(index)]);
			showMapLists();
		}
		return Value();
	});
	// An index into mapListChosen.
	n.setFunction(kMultiplay, "removeMapfromMapList", [this](Interpreter& vm, const Args& args) {
		const double index = vm.toNumber(arg(args, 0));
		if (index >= 0 && index < static_cast<double>(m_mapList.size())) {
			m_mapList.erase(m_mapList.begin() + static_cast<long>(index));
			showMapLists();
		}
		return Value();
	});
	// 0x100459f0: the level's name with spaces for underscores.
	n.setFunction(kMultiplay, "getMapNameFromId", [](Interpreter& vm, const Args& args) {
		std::string name = vm.toString(arg(args, 0));
		std::replace(name.begin(), name.end(), '_', ' ');
		return Value(name);
	});
	// "Level size": the level's picture for its mode and size.
	n.setFunction(kMultiplay, "getMapImage", [this](Interpreter& vm, const Args& args) {
		std::istringstream words(vm.toString(arg(args, 0)));
		std::string level, size;
		words >> level >> size;
		return Value("/Levels/" + level + "/Info/" + m_filterGameMode + "_" + size + "_menuMap.png");
	});
	n.setFunction(kMultiplay, "getMapMovie", [](Interpreter&, const Args&) { return Value(""); });
	n.setFunction(kMultiplay, "isMovieComplete", [](Interpreter&, const Args&) { return Value(true); });
	n.setFunction(kMultiplay, "createServer", [this](Interpreter&, const Args&) {
		const GameSetup setup = gameSetup();
		if (setup.maps.empty()) {
			log("create server: no map chosen");
			return Value(false);
		}
		log("create server: " + setup.maps.front().level + " " + setup.maps.front().gameMode + " "
			+ std::to_string(setup.maps.front().size) + ", " + std::to_string(setup.bots) + " bots");
		if (onStartGame) {
			onStartGame(setup);
		}
		return Value(true);
	});
	// The name of the server the info popup shows (its lists got the
	// address as arguments).
	n.setFunction(kServerInfo, "getServerName", [this](Interpreter&, const Args&) {
		for (const Server& server : m_servers) {
			if (server.entry.ip == m_infoHost) {
				return Value(field(server.entry, "hostname"));
			}
		}
		return Value("");
	});
}

// --- dice.bf2.ServerSettings (0x10055xxx) and dice.bf2.GeneralSettings

// The pages read each setting with getX and write it with setX; apply()
// makes them the server's. Kept here by name; createServer reads them.
void MenuBindings::bindServerSettings()
{
	ui::NativeRegistry& n = m_natives;
	// What a setting is before the menu sets it (the game's defaults).
	static const std::map<std::string, Value> defaults = {
		{"ServerSettings.MaxPlayers", Value(16)},
		{"ServerSettings.RoundsPerMap", Value(1)},
		{"ServerSettings.TicketRatio", Value(100)},
		{"ServerSettings.TeamRatio", Value(100)},
		{"ServerSettings.SpawnTime", Value(15)},
		{"ServerSettings.FriendlyFire", Value(true)},
		{"ServerSettings.AutoBalanceTeam", Value(true)},
		{"ServerSettings.ServerName", Value("BF2142 server")},
		{"ServerSettings.GameMode", Value("gpm_sp")},
		{"GeneralSettings.MaxBots", Value(16)},
		{"GeneralSettings.BotSkill", Value(0.5)},
		{"GeneralSettings.UseBots", Value(true)},
		{"GeneralSettings.MaxBotsIncludeHumans", Value(true)},
		// The video page: qualities 1 low, 2 medium, 3 high (0 off for the
		// dynamic ones); antialiasing by its VIDEOSETTINGS_ name.
		{"VideoSettings.TerrainQuality", Value(3)},
		{"VideoSettings.EffectsQuality", Value(3)},
		{"VideoSettings.GeometryQuality", Value(3)},
		{"VideoSettings.TextureQuality", Value(3)},
		{"VideoSettings.LightingQuality", Value(3)},
		{"VideoSettings.DynamicShadowsQuality", Value(2)},
		{"VideoSettings.DynamicLightingQuality", Value(2)},
		{"VideoSettings.TextureFilteringQuality", Value(3)},
		{"VideoSettings.ViewDistanceScale", Value(1)},
		{"VideoSettings.Antialiasing", Value("Off")},
		{"VideoSettings.BloomEnabled", Value(true)},
		{"VideoSettings.VideoOptionScheme", Value(3)},
		{"VideoSettings.Resolution", Value("1280x720@60Hz")},
		{"AudioSettings.EffectsVolume", Value(1)},
		{"AudioSettings.MusicVolume", Value(1)},
		{"AudioSettings.VoiceOverVolume", Value(1)},
		{"AudioSettings.NumberOfVoices", Value(32)},
		{"AudioSettings.SoundQuality", Value(2)},
		{"AudioSettings.SoundDetail", Value(2)},
		{"AudioSettings.Provider", Value("Software")},
		{"ControlSettings.MouseSensitivity", Value(0.5)},
		{"ControlSettings.MouseSensitivityMultiplier", Value(1)},
		{"ControlSettings.MouseYAWFactor", Value(1)},
		{"ControlSettings.MousePITCHFactor", Value(1)},
		{"ControlSettings.InvertMouse", Value(false)},
	};
	for (const char* object : kSettingsObjects) {
		const std::string prefix = std::string(object).substr(std::string("dice.bf2.").size()) + ".";
		for (const std::string& member : n.members(object)) {
			if (member.size() <= 3 || n.slotType(object, member) != ui::NativeSlot::Function) {
				continue;
			}
			const std::string verb = member.substr(0, 3), key = prefix + member.substr(3);
			if (verb == "set") {
				n.setFunction(object, member, [this, key](Interpreter&, const Args& args) {
					m_settings[key] = arg(args, 0);
					return Value();
				});
			} else if (verb == "get") {
				n.setFunction(object, member, [this, key](Interpreter&, const Args&) {
					if (const auto it = m_settings.find(key); it != m_settings.end()) {
						return it->second;
					}
					const auto it = defaults.find(key);
					return it == defaults.end() ? Value(0) : it->second;
				});
			}
		}
		// apply() keeps the page's values (and writes them down), cancel()
		// goes back to the last applied ones, reset() to the defaults.
		n.setFunction(object, "apply", [this, prefix](Interpreter&, const Args&) {
			for (const auto& [key, value] : m_settings) {
				if (key.compare(0, prefix.size(), prefix) == 0) {
					m_appliedSettings[key] = value;
				}
			}
			saveSettings();
			if (onSettingsApplied) {
				onSettingsApplied();
			}
			return Value();
		});
		n.setFunction(object, "cancel", [this, prefix](Interpreter&, const Args&) {
			for (auto it = m_settings.begin(); it != m_settings.end();) {
				it = it->first.compare(0, prefix.size(), prefix) == 0 ? m_settings.erase(it) : std::next(it);
			}
			for (const auto& [key, value] : m_appliedSettings) {
				if (key.compare(0, prefix.size(), prefix) == 0) {
					m_settings[key] = value;
				}
			}
			return Value();
		});
		n.setFunction(object, "reset", [this, prefix](Interpreter&, const Args&) {
			for (auto it = m_settings.begin(); it != m_settings.end();) {
				it = it->first.compare(0, prefix.size(), prefix) == 0 ? m_settings.erase(it) : std::next(it);
			}
			for (const auto& [key, value] : defaults) {
				if (key.compare(0, prefix.size(), prefix) == 0) {
					m_settings[key] = value;
				}
			}
			return Value();
		});
		n.setFunction(object, "canApply", [](Interpreter&, const Args&) { return Value(true); });
		n.setFunction(object, "needsRestart", [](Interpreter&, const Args&) { return Value(false); });
	}
	// 0x1005cc20: the antialiasing modes, {value, text} each.
	n.setFunction(kVideoSettings, "getAvailableAntialiasing", [this](Interpreter& vm, const Args&) {
		std::vector<Value> modes;
		for (const char* mode : {"Off", "2Samples", "4Samples", "8Samples"}) {
			auto object = vm.newObject();
			vm.setMember(Value(object), "value", Value(mode));
			vm.setMember(Value(object), "text", Value(m_natives.localize(std::string("VIDEOSETTINGS_") + mode)));
			modes.push_back(Value(object));
		}
		return Value(vm.newArray(std::move(modes)));
	});
	// 0x1005d2d0: the display mode in use, as the displayModes list writes it.
	n.setFunction(kVideoSettings, "getSelectedResolution", [this](Interpreter&, const Args&) {
		return m_settings.count("VideoSettings.Resolution") ? m_settings.at("VideoSettings.Resolution") : Value("");
	});
	// VoIP (no voice chat yet): off, the volumes at half.
	n.setFunction("dice.bf2.Sound", "voipSetEnable", [](Interpreter&, const Args&) { return Value(); });
	for (const char* member : {"voipInitialize", "voipStopTest", "voipStartTest", "voipSetBoost", "voipSetTransmitVolume",
			 "voipSetRecieveVolume", "voipSetThresholdVolume"}) {
		n.setFunction("dice.bf2.Sound", member, [](Interpreter&, const Args&) { return Value(); });
	}
	n.setFunction("dice.bf2.Sound", "voipGetEnable", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction("dice.bf2.Sound", "voipGetBoost", [](Interpreter&, const Args&) { return Value(false); });
	for (const char* member : {"voipGetTransmitVolume", "voipGetRecieveVolume", "voipGetThresholdVolume"}) {
		n.setFunction("dice.bf2.Sound", member, [](Interpreter&, const Args&) { return Value(0.5); });
	}
	for (const auto& [key, value] : defaults) {
		m_settings.emplace(key, value);
	}
	m_appliedSettings = m_settings;
}

// The settings file: "Object.Name type value" per line (n number, b
// boolean, s text), the applied values of every settings object.
void MenuBindings::setSettingsFile(const std::string& path)
{
	m_settingsFile = path;
	std::ifstream in(path);
	for (std::string line; std::getline(in, line);) {
		std::istringstream words(line);
		std::string key, type, value;
		words >> key >> type;
		std::getline(words >> std::ws, value);
		if (key.empty()) {
			continue;
		}
		const Value v = type == "n" ? Value(std::atof(value.c_str())) : type == "b" ? Value(value == "1") : Value(value);
		m_settings[key] = v;
		m_appliedSettings[key] = v;
	}
}

void MenuBindings::saveSettings() const
{
	if (m_settingsFile.empty()) {
		return;
	}
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(m_settingsFile).parent_path(), error);
	std::ofstream out(m_settingsFile);
	for (const auto& [key, v] : m_appliedSettings) {
		if (v.isNumber()) {
			out << key << " n " << avm1::numberToString(v.asNumber()) << "\n";
		} else if (v.isBoolean()) {
			out << key << " b " << (v.asBoolean() ? 1 : 0) << "\n";
		} else if (v.isString()) {
			out << key << " s " << v.asString() << "\n";
		}
	}
}

std::string MenuBindings::defaultSettingsFile()
{
	const std::string profiles = defaultProfileFile();
	return profiles.empty() ? std::string() : (std::filesystem::path(profiles).parent_path() / "settings.txt").string();
}

std::string MenuBindings::text(const std::string& name) const
{
	const auto it = m_settings.find(name);
	return it != m_settings.end() && it->second.isString() ? it->second.asString() : std::string();
}

// --- dice.bf2.ControlSettings: the controls page

void MenuBindings::bindControls()
{
	ui::NativeRegistry& n = m_natives;
	// The page's maps by number (setControlMap 0..4): on foot, land
	// vehicles, aircraft, helicopters, ships (provisional: Controls.con's
	// order).
	static const char* kMaps[] = {input::ControlMaps::kInfantry, input::ControlMaps::kLand, input::ControlMaps::kAir,
		input::ControlMaps::kHelicopter, input::ControlMaps::kSea};
	const auto mapName = [this] { return std::string(kMaps[std::clamp(m_controlMap, 0, 4)]); };
	n.setFunction(kControlSettings, "setActiveControlMap", [this](Interpreter& vm, const Args& args) {
		m_controlMap = static_cast<int>(vm.toNumber(arg(args, 0)));
		return Value();
	});
	n.setFunction(kControlSettings, "getActiveControlMap", [this](Interpreter&, const Args&) { return Value(m_controlMap); });
	// getMappedString(keyId, secondary, negative): the key on the input
	// (keyId: the engine's c_PI number) in that slot and side, as shown.
	n.setFunction(kControlSettings, "getMappedString", [this, mapName](Interpreter& vm, const Args& args) {
		if (!m_controls) {
			return Value("");
		}
		const std::string input = input::inputNamed(static_cast<int>(vm.toNumber(arg(args, 0))), true);
		const input::Binding* b =
			m_controls->binding(mapName(), input, avm1::toBoolean(arg(args, 1)) ? 1 : 0, avm1::toBoolean(arg(args, 2)) ? -1.0f : 1.0f);
		if (!b) {
			return Value("");
		}
		// "IDKey_W" as "W", "IDButton_0" as "Mouse 1".
		if (b->control.compare(0, 9, "IDButton_") == 0) {
			return Value("Mouse " + std::to_string(std::atoi(b->control.c_str() + 9) + 1));
		}
		return Value(b->control.compare(0, 6, "IDKey_") == 0 ? b->control.substr(6) : b->control);
	});
	// mapInput(keyId, secondary, negative, superceed): binds the key pressed
	// last there; true when it was bound.
	n.setFunction(kControlSettings, "mapInput", [this, mapName](Interpreter& vm, const Args& args) {
		const std::string input = input::inputNamed(static_cast<int>(vm.toNumber(arg(args, 0))), true);
		if (!m_controls || input.empty() || m_lastInput.empty()) {
			return Value(false);
		}
		m_controls->bind(mapName(), input, m_lastInput, avm1::toBoolean(arg(args, 1)) ? 1 : 0,
			avm1::toBoolean(arg(args, 2)) ? -1.0f : 1.0f);
		log("controls: " + input + " on " + m_lastInput);
		return Value(true);
	});
	n.setFunction(kControlSettings, "getLastDuplicateInput", [](Interpreter&, const Args&) { return Value(""); });
	n.setFunction(kControlSettings, "forceSetLastInput", [](Interpreter&, const Args&) { return Value(); });
}

double MenuBindings::setting(const std::string& name, double fallback) const
{
	const auto it = m_settings.find(name);
	if (it == m_settings.end()) {
		return fallback;
	}
	const Value& v = it->second;
	if (v.isBoolean()) {
		return v.asBoolean() ? 1 : 0;
	}
	if (v.isNumber()) {
		return v.asNumber();
	}
	if (v.isString()) {
		return std::atof(v.asString().c_str());
	}
	return fallback;
}

void MenuBindings::setDisplayModes(const std::vector<std::string>& modes)
{
	ui::NativeRegistry::Rows rows;
	for (const std::string& mode : modes) {
		rows.push_back({mode});
	}
	m_natives.setListRows("displayModes", rows);
}

void MenuBindings::setMaps(std::vector<MenuMap> maps)
{
	std::sort(maps.begin(), maps.end(), [](const MenuMap& a, const MenuMap& b) {
		return std::tie(a.level, a.gameMode, a.size) < std::tie(b.level, b.gameMode, b.size);
	});
	m_maps = std::move(maps);
	showMapLists();
}

std::vector<MenuMap> MenuBindings::availableMaps() const
{
	std::vector<MenuMap> maps;
	for (const MenuMap& map : m_maps) {
		if (map.gameMode == m_filterGameMode) {
			maps.push_back(map);
		}
	}
	return maps;
}

// 0x100723c0 / 0x10072a70: "Level Name - size" (underscores as spaces).
void MenuBindings::showMapLists()
{
	const auto rows = [](const std::vector<MenuMap>& maps) {
		ui::NativeRegistry::Rows rows;
		for (const MenuMap& map : maps) {
			std::string name = map.level;
			std::replace(name.begin(), name.end(), '_', ' ');
			rows.push_back({name + " - " + std::to_string(map.size)});
		}
		return rows;
	};
	m_natives.setListRows("mapListAvailable", rows(availableMaps()));
	m_natives.setListRows("mapListChosen", rows(m_mapList));
}

GameSetup MenuBindings::gameSetup() const
{
	GameSetup setup;
	setup.maps = m_mapList;
	setup.maxPlayers = static_cast<int>(setting("ServerSettings.MaxPlayers", 16));
	const bool useBots = setting("GeneralSettings.UseBots", 1) != 0;
	setup.bots = useBots ? static_cast<int>(setting("GeneralSettings.MaxBots", 16)) : 0;
	setup.botsIncludeHumans = setting("GeneralSettings.MaxBotsIncludeHumans", 1) != 0;
	float skill = static_cast<float>(setting("GeneralSettings.BotSkill", 0.5));
	if (skill > 1) {
		skill /= 100; // a percentage
	}
	setup.botSkill = std::clamp(skill, 0.0f, 1.0f);
	setup.ticketRatio = static_cast<int>(setting("ServerSettings.TicketRatio", 100));
	setup.spawnTime = static_cast<int>(setting("ServerSettings.SpawnTime", 15));
	setup.timeLimit = static_cast<int>(setting("ServerSettings.TimeLimit", 0));
	setup.scoreLimit = static_cast<int>(setting("ServerSettings.ScoreLimit", 0));
	setup.friendlyFire = setting("ServerSettings.FriendlyFire", 1) != 0;
	setup.roundsPerMap = std::max(1, static_cast<int>(setting("ServerSettings.RoundsPerMap", 1)));
	return setup;
}

} // namespace dice::hfe::menu
