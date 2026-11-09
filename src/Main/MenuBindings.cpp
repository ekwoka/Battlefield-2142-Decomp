#include "MenuBindings.h"

#include "Console/Console.h"
#include "UI/Player/Scroller.h"

#include "Net/ServerQuery.h"
#include "Net/ServerSettings.h"

#include "EndOfRoundBindings.h"
#include "Input/ControlMap.h"
#include "Input/GameControllers.h"
#include "SettingsObjects.h"
#include "Net/Fesl.h"
#include "Net/OnlineConfig.h"
#include "Net/OnlineSession.h"
#include "Net/ServerQuery.h"
#include "Net/Socket.h"
#include "UI/Player/Natives.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
	std::vector<net::ServerEntry> lan;
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
	// soldier login, and open while the game runs (net::OnlineSession: FESL,
	// then GameSpy Presence with the pre-authentication ticket). Where it
	// goes is net::onlineConfig(): the local emulators unless --online.
	auto session = std::make_shared<net::OnlineSession>();
	session->onLog = [](const std::string& line) { std::printf("online: %s\n", line.c_str()); };

	MenuServices services;
	services.browseLan = [] { return net::browseLan(); };
	services.checkOnline = []() {
		const net::ServiceEndpoint fesl = net::onlineConfig().endpoints.fesl;
		if (!net::hostAllowed(fesl.host)) {
			return false;
		}
		net::TcpSocket socket;
		return socket.connect(fesl.host, fesl.port, 5);
	};
	services.loginAccount = [session](const std::string& name, const std::string& password) {
		AccountReply out;
		std::string error;
		long code = 0;
		const auto account = session->loginAccount(name, password, &error, &code);
		if (!account) {
			out.reached = code != 0;
			out.error = code != 0 ? code : 99;
			return out;
		}
		out.soldiers = account->soldiers;
		return out;
	};
	services.loginSoldier = [session](const std::string& soldier) {
		AccountReply out;
		std::string error;
		long code = 0;
		if (!session->loginSoldier(soldier, &error, &code)) {
			out.reached = code != 0;
			out.error = code != 0 ? code : 99;
			std::printf("online: %s\n", error.c_str());
			return out;
		}
		const net::OnlineIdentity identity = session->identity();
		out.profileId = identity.profileId;
		out.nameChecksum = identity.nameChecksum;
		out.loginTicket = identity.loginTicket;
		session->keepAlive();
		return out;
	};
	services.logoutAccount = [session]() { session->logout(); };
	services.fetchServers = [](std::string* error) {
		// The fields the game's browser asks for.
		const std::vector<std::string> fields {"hostname", "gamever", "mapname", "gametype", "gamevariant", "numplayers",
			"maxplayers", "password", "hostport", "bf2142_pure", "bf2142_ranked", "bf2142_ranked_tournament",
			"bf2142_reservedslots", "bf2142_sponsortext", "bf2142_communitylogo_url", "bf2142_sponsorlogo_url",
			"bf2142_maxrank"};
		const net::ServiceEndpoint master = net::onlineConfig().endpoints.master;
		return net::fetchServerList(master.host, "", fields, 15, error, master.port);
	};
	services.connectServer = [](const std::string& host, uint16_t port) {
		net::GameConnection connection;
		if (!net::hostAllowed(host)) {
			net::ConnectResult refused;
			refused.outcome = net::ConnectResult::Outcome::Error;
			return refused;
		}
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
	bindOffline();
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
			&& !Job::running(m_joinProbe) && !Job::running(m_lanQuery)) {
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
	n.setFunction(kLogic, "getModVersion", [](Interpreter&, const Args&) { return Value(kModVersion); });
	// 0x1003d370: today as "%04d%02d%02d" (local time).
	n.setFunction(kLogic, "getDateString", [](Interpreter&, const Args&) {
		const std::time_t now = std::time(nullptr);
		const std::tm* t = std::localtime(&now);
		char text[16] = "";
		if (t) {
			std::snprintf(text, sizeof text, "%04d%02d%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
		}
		return Value(std::string(text));
	});
	// 0x1003b620: 12.
	n.setFunction(kLogic, "getMaxRank", [](Interpreter&, const Args&) { return Value(12); });
	// No patch server to ask: the check is over at once and finds nothing.
	n.setFunction(kLogic, "checkVersion", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "getVersionResult", [](Interpreter&, const Args&) { return Value(0); });
	n.setFunction(kLogic, "getEADownloader", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "isGameRunning", [this](Interpreter&, const Args&) { return Value(m_gameRunning); });
	n.setFunction(kLogic, "disconnectGame", [this](Interpreter&, const Args&) {
		log("disconnect");
		m_currentGame.reset();
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
		// 0x1007ab70: offline, the soldiers are the sub accounts of this
		// account's local profiles.
		if (m_profiles) {
			for (const auto& entry : m_profiles->list()) {
				if (!entry.sub.empty() && entry.master.size() == name.size()
					&& std::equal(name.begin(), name.end(), entry.master.begin(), [](char a, char b) {
						   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
					   })
					&& std::find(m_account.soldiers.begin(), m_account.soldiers.end(), entry.sub) == m_account.soldiers.end()) {
					m_account.soldiers.push_back(entry.sub);
				}
			}
		}
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
	// Awards (0x1004f690-0x100506f0): the held-award list comes from the
	// stats server (getawardsinfo.aspx); offline none is held, so every
	// award shows its locked picture (_bw, a badge's level 0).
	n.setFunction(kProfile, "getLatestAwards", [](Interpreter& vm, const Args&) {
		return Value(vm.newArray()); // {id, path} objects of the newest held awards: none
	});
	n.setFunction(kProfile, "selectAward", [this](Interpreter& vm, const Args& args) {
		m_selectedAward = vm.toString(arg(args, 0));
		return Value(0); // 1 when held
	});
	n.setFunction(kProfile, "selectLatestAward", [](Interpreter&, const Args&) { return Value(0); });
	n.setFunction(kProfile, "hasAward", [](Interpreter&, const Args&) { return Value(0); });
	n.setFunction(kProfile, "hasThisAward", [](Interpreter&, const Args&) { return Value(0); });
	n.setFunction(kProfile, "getAwardLevel", [](Interpreter&, const Args&) { return Value(0); });
	n.setFunction(kProfile, "getAwardFirstDate", [](Interpreter&, const Args&) { return Value(""); });
	n.setFunction(kProfile, "getAwardLatestDate", [](Interpreter&, const Args&) { return Value(""); });
	n.setFunction(kProfile, "getAwardName", [this](Interpreter&, const Args&) {
		// AWARD_NAME_<id>, a badge's by its first level.
		const std::string id = m_selectedAward;
		const bool badge = std::atoi(id.c_str()) > 0 && std::atoi(id.c_str()) < 200 && id.find('_') == std::string::npos;
		return Value(m_natives.localize("AWARD_NAME_" + id + (badge ? "_1" : "")));
	});
	n.setFunction(kProfile, "getAwardDescription", [](Interpreter&, const Args&) { return Value(""); });
	// The progress chart (Logic.setActiveDiagram 0x1003c0e0 → the game's
	// +0x3c4, drawing getplayerprogress.aspx's series into the clip): no
	// stats server, no series.
	n.setFunction(kLogic, "setActiveDiagram", [](Interpreter&, const Args&) { return Value(); });
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
	// Time played as the stats rows show times (0x1007ada0: TEXT_timeformat_hhmm).
	const std::string played =
		hoursMinutesFromSeconds(m_natives.localize("TEXT_timeformat_hhmm"), static_cast<int>(s.timePlayed));
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
			takeProfile(m_account.name, soldier);
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
		if (m_profiles) {
			m_profiles->release();
		}
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
		if (m_profiles) {
			m_profiles->release();
		}
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
		// 0x1004d280: createProfile(the last argument, offline, reset); the
		// new profile is current (the page goes on to "start").
		const std::string name = vm.toString(args.empty() ? Value() : args.back());
		if (m_profiles) {
			if (m_profiles->create(name, true, true) < 0) {
				return Value(false);
			}
			m_offline = true;
			m_account = {};
			m_account.name = name;
			m_account.loggedIn = true;
			m_soldier = name;
			showLocalProfiles();
			profileLoaded();
			log("offline profile " + name + " in " + m_profiles->currentDirectory());
			return Value(true);
		}
		if (name.empty() || localAccount(name)) {
			return Value(false);
		}
		m_local.push_back({name, {name}, {}});
		saveProfiles();
		return Value(true);
	});
	n.setFunction(kProfile, "loginOfflineAccount", [this](Interpreter& vm, const Args& args) {
		const int index = static_cast<int>(vm.toNumber(arg(args, 0)));
		// 0x1004d450: select(index), the row's profileIndex.
		if (m_profiles) {
			const auto entries = m_profiles->list();
			if (index < 0 || index >= static_cast<int>(entries.size()) || !m_profiles->select(index)) {
				return Value(false);
			}
			m_offline = true;
			m_account = {};
			m_account.name = entries[static_cast<size_t>(index)].name;
			m_account.loggedIn = true;
			m_soldier = m_account.name;
			profileLoaded();
			log("offline profile " + m_account.name + " from " + m_profiles->currentDirectory());
			return Value(true);
		}
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
		// 0x10050fb0: all the profiles.
		return Value(static_cast<double>(m_profiles ? m_profiles->list().size() : m_local.size()));
	});
	// 0x10050cf0 / 0x10050e50: those with and without a GameSpy nick.
	for (const bool withNick : {true, false}) {
		n.setFunction(kProfile, withNick ? "getNumOnlineProfiles" : "getNumOfflineProfiles", [this, withNick](Interpreter&, const Args&) {
			double count = 0;
			if (m_profiles) {
				for (const auto& entry : m_profiles->list()) {
					count += entry.gamespyNick.empty() != withNick ? 1 : 0;
				}
			}
			return Value(count);
		});
	}
	// 0x1004c720: delete(index); refused while a profile is current.
	n.setFunction(kProfile, "deleteLocalProfile", [this](Interpreter& vm, const Args& args) {
		const int index = static_cast<int>(vm.toNumber(arg(args, 0)));
		const bool removed = m_profiles && m_profiles->remove(index);
		if (removed) {
			showLocalProfiles();
		}
		return Value(removed);
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
	// 0x1004bb20: Global.con's last online user.
	if (m_objects) {
		m_objects->set("GlobalSettings.LastOnlineUser", m_account.name);
	}
	std::string soldiers;
	for (const std::string& s : reply.soldiers) {
		soldiers += " " + s;
	}
	log("logged in to account " + m_account.name + "; soldiers:" + soldiers);
}

void MenuBindings::finishSoldierLogin()
{
	m_transactionKind = Transaction::None;
	AccountReply login;
	if (m_transaction) {
		const AccountReply reply = m_transaction->account;
		login = reply;
		m_transaction.reset();
		m_lastFeslError = reply.error;
		m_lastError = reply.reached ? profileErrorFromFesl(reply.error) : kProfileErrorNoConnection;
		if (m_lastError != kProfileErrorNone) {
			log("soldier " + m_transactionName + " failed: " + profileErrorKey(m_lastError));
			return;
		}
	}
	m_soldier = m_transactionName;
	m_soldierLogin = login;
	log("playing as " + m_soldier + (m_account.connected ? "" : " (offline)"));
	if (m_account.connected) {
		takeProfile(m_account.name, m_soldier);
	}
}

void MenuBindings::takeProfile(const std::string& account, const std::string& soldier)
{
	if (!m_profiles || !m_objects) {
		return;
	}
	m_profiles->release();
	// Online the profile of the soldier (0x00491030, its sub account);
	// offline the one of this account's soldier (0x100510c0).
	int index = -1;
	const auto entries = m_profiles->list();
	for (size_t i = 0; i < entries.size() && index < 0; ++i) {
		if (entries[i].sub == soldier && (m_account.connected || entries[i].master == account)) {
			index = static_cast<int>(i);
		}
	}
	if (index >= 0) {
		m_profiles->select(index);
	} else {
		// createProfile(soldier, online, reset): master the last online user.
		m_objects->set("GlobalSettings.LastOnlineUser", account);
		m_profiles->create(soldier, false, true);
	}
	// The soldier's standing (rank and points from the stats) and the time.
	m_objects->set("LocalProfile.LastBaseUpdate", std::to_string(static_cast<long long>(std::time(nullptr))));
	m_profiles->save();
	log("profile " + m_profiles->currentDirectory() + " for " + soldier);
	profileLoaded();
}

void MenuBindings::profileLoaded()
{
	if (m_objects) {
		setSettingsObjects(m_objects);
	}
	if (onSettingsApplied) {
		onSettingsApplied();
	}
}

void MenuBindings::setLocalProfiles(LocalProfileManager* profiles)
{
	m_profiles = profiles;
	showLocalProfiles();
}

void MenuBindings::showLocalProfiles()
{
	if (!m_profiles) {
		return;
	}
	std::vector<std::vector<std::string>> rows;
	m_localProfileIndices.clear();
	const auto entries = m_profiles->list();
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].master.empty() && entries[i].gamespyNick.empty()) {
			rows.push_back({entries[i].name});
			m_localProfileIndices.push_back(static_cast<int>(i));
		}
	}
	m_natives.setListRows("localProfiles", rows);
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
	// BF2142_CONFIG_DIR stands for <config dir>/bf2142-recomp (scratch runs).
	if (const char* dir = std::getenv("BF2142_CONFIG_DIR"); dir && *dir) {
		return (std::filesystem::path(dir) / "profiles.txt").string();
	}
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
	if (m_profiles) {
		showLocalProfiles();
	} else {
		m_natives.setListRows("localProfiles", rows);
	}
}

void MenuBindings::saveProfiles()
{
	std::vector<std::vector<std::string>> rows;
	for (const LocalAccount& local : m_local) {
		rows.push_back({local.name});
	}
	if (m_profiles) {
		showLocalProfiles();
	} else {
		m_natives.setListRows("localProfiles", rows);
	}
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
		refreshFavourites(); // their rows take the listed fields; shows the lists
	}
	return false;
}

namespace {

// A server's row (SwiffPlayer.dll 0x10079770's columns; the browsers' row
// fills 0x100760a0 and 0x10077c90 write the same cells, "%d/%d (%d)", and set
// the row's icons, as `show` in bindLists does).
std::vector<std::string> serverRow(const ui::NativeRegistry& natives, const net::ServerEntry& e)
{
	std::string map = field(e, "mapname");
	std::replace(map.begin(), map.end(), '_', ' ');
	const std::string type = field(e, "gametype");
	const bool listed = e.fields.count("maxplayers") != 0; // a favourite the browser has not listed: no counts
	return {field(e, "hostname"),
		listed ? std::to_string(fieldNumber(e, "numplayers")) + "/" + std::to_string(fieldNumber(e, "maxplayers")) + " (0)" : std::string(),
		"", map, type.empty() ? std::string() : natives.localize("TEXT_" + type)};
}

} // namespace

void MenuBindings::showServers()
{
	std::vector<std::vector<std::string>> rows;
	for (const Server& server : m_servers) {
		rows.push_back(serverRow(m_natives, server.entry));
	}
	// 0x10035e70: "Favorites", the favourites (the "favourites" browser's,
	// row - 1), then "Live servers" and the rest.
	std::vector<std::vector<std::string>> withHeaders {{m_natives.localize("SERVERBROWSER_favorites")}};
	for (const Server& server : m_favouriteServers) {
		withHeaders.push_back(serverRow(m_natives, server.entry));
	}
	withHeaders.push_back({});
	withHeaders.push_back({m_natives.localize("SERVERBROWSER_liveservers")});
	withHeaders.insert(withHeaders.end(), rows.begin(), rows.end());
	m_natives.setListRows("serverListQuick", withHeaders);
	m_natives.setListRows("serverListAdvanced", withHeaders);
	m_natives.setListRows("serverList", rows);
}

void MenuBindings::fillAwardProgress(const std::string& id)
{
	// 0x1005fde0: the in-round criteria under HEADER_inaround (rows 1-2,
	// the text and the target), the career ones under HEADER_global from
	// row 4 (the text, value/target, a bar or a check mark).
	const stats::AwardDefinition* award = nullptr;
	for (const stats::AwardDefinition& a : m_awards) {
		award = a.id == id ? &a : award;
	}
	ui::NativeRegistry::Rows rows;
	m_awardRows.clear();
	if (award) {
		std::vector<const stats::AwardCriterion*> round, global;
		for (const stats::AwardCriterion& c : award->criteria) {
			(c.global ? global : round).push_back(&c);
		}
		const auto number = [](double v) {
			char text[32];
			std::snprintf(text, sizeof(text), v == std::floor(v) ? "%.0f" : "%.2f", v);
			return std::string(text);
		};
		stats::SoldierStats recruit;
		const stats::SoldierStats* record = soldierStats();
		const stats::SoldierStats& me = record ? *record : recruit;
		const size_t count = global.empty() ? round.size() + 1 : global.size() + 4;
		rows.resize(count, {"", "", ""});
		m_awardRows.resize(count);
		if (!round.empty()) {
			rows[0][0] = m_natives.localize("HEADER_inaround");
		}
		for (size_t i = 0; i < round.size() && i < 2; ++i) {
			rows[i + 1][1] = m_natives.localize("AWARD_CRITERIA_" + std::to_string(round[i]->textId));
			rows[i + 1][2] = round[i]->target > 0 ? number(round[i]->target) : std::string();
		}
		if (!global.empty()) {
			rows[3][0] = m_natives.localize("HEADER_global");
		}
		for (size_t i = 0; i < global.size(); ++i) {
			const stats::AwardCriterion& c = *global[i];
			double value = 0;
			for (const std::string& key : c.keys) {
				value += std::isdigit(static_cast<unsigned char>(key[0])) ? 0.0 : stats::awardStatValue(me, key); // no award is held
			}
			const double target = c.whole && c.target == 0 ? 1 : c.target;
			rows[i + 4][1] = m_natives.localize("AWARD_CRITERIA_" + std::to_string(c.textId));
			if (c.check) {
				m_awardRows[i + 4].check = value >= target;
			} else {
				m_awardRows[i + 4].progress = target > 0 ? static_cast<float>(std::min(value / target, 1.0)) : 0;
				rows[i + 4][2] = number(value) + "/" + number(target);
			}
		}
	}
	m_natives.setListRows("awardProgress", rows);
}

const MenuBindings::Server* MenuBindings::serverAt(const std::string& listId, int row, bool* favourite) const
{
	if (favourite) {
		*favourite = false;
	}
	const auto at = [row](const std::vector<Server>& servers) {
		return row >= 0 && row < static_cast<int>(servers.size()) ? &servers[static_cast<size_t>(row)] : nullptr;
	};
	if (listId == "serverListLAN") {
		return at(m_lanServers);
	}
	if (listId == "serverHistory") {
		return at(m_historyServers);
	}
	if (listId == "favouriteServers") {
		if (favourite) {
			*favourite = true;
		}
		return at(m_favouriteServers);
	}
	if (listId == "serverList") {
		return at(m_servers);
	}
	if (!hasHeaderRows(listId) || isHeaderRow(listId, row)) {
		return nullptr;
	}
	const int favourites = static_cast<int>(m_favouriteServers.size());
	if (row <= favourites) {
		if (favourite) {
			*favourite = true;
		}
		return &m_favouriteServers[static_cast<size_t>(row - 1)];
	}
	const int live = row - favourites - kHeaderRows;
	return live >= 0 && live < static_cast<int>(m_servers.size()) ? &m_servers[static_cast<size_t>(live)] : nullptr;
}

bool MenuBindings::isHeaderRow(const std::string& listId, int row) const
{
	const int favourites = static_cast<int>(m_favouriteServers.size());
	return hasHeaderRows(listId) && (row == 0 || row == favourites + 1 || row == favourites + 2);
}

void MenuBindings::startLanQuery()
{
	if (Job::running(m_lanQuery)) {
		return;
	}
	const auto browse = m_services.browseLan;
	m_lanQuery = Job::start([browse](Job& job) {
		if (browse) {
			job.lan = browse();
		}
	});
	log("searching the local network");
}

bool MenuBindings::finishLanQuery()
{
	if (Job::running(m_lanQuery)) {
		return true;
	}
	if (m_lanQuery) {
		m_lanServers.clear();
		for (const net::ServerEntry& entry : m_lanQuery->lan) {
			Server server;
			server.entry = entry;
			const int port = fieldNumber(entry, "hostport");
			server.gamePort = static_cast<uint16_t>(port > 0 ? port : net::kGamePort);
			m_lanServers.push_back(std::move(server));
		}
		log(std::to_string(m_lanServers.size()) + " servers on the local network");
		m_lanQuery.reset();
		showLanServers();
		refreshFavourites();
	}
	return false;
}

void MenuBindings::showLanServers()
{
	std::vector<std::vector<std::string>> rows;
	for (const Server& server : m_lanServers) {
		rows.push_back(serverRow(m_natives, server.entry));
	}
	m_natives.setListRows("serverListLAN", rows);
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
		if (list.listId == "serverExtendedInfo" && list.holder) {
			// 0x10068380: bg1 shows on the even rows of a known server's
			// rules; no row is clickable.
			const std::string prefix = list.rowPrefix(vm);
			for (int i = 0; i < list.visibleRows(vm); ++i) {
				const Value row = vm.getMember(Value(list.holder), prefix + std::to_string(i));
				const int index = list.firstIndex + i;
				const Value bg = vm.getMember(row, "bg1");
				if (bg.isObject()) {
					vm.setMember(bg, "_visible", Value(index < m_serverInfoRows && index % 2 == 0));
				}
				vm.setMember(row, "isClickable", Value(0));
			}
			return;
		}
		if (list.listId == "awardProgress" && list.holder) {
			// The rows' check mark and bar (0x1005f1e0).
			const std::string prefix = list.rowPrefix(vm);
			for (int i = 0; i < list.visibleRows(vm); ++i) {
				const Value row = vm.getMember(Value(list.holder), prefix + std::to_string(i));
				const size_t index = static_cast<size_t>(list.firstIndex + i);
				const AwardRow state = index < m_awardRows.size() ? m_awardRows[index] : AwardRow {};
				const Value check = vm.getMember(row, "mcIconCheck");
				if (check.isObject()) {
					vm.setMember(check, "_visible", Value(state.check));
				}
				const Value bar = vm.getMember(row, "mcProgress");
				if (bar.isObject()) {
					vm.setMember(bar, "_visible", Value(state.progress >= 0));
					if (state.progress >= 0) {
						vm.callMethod(bar.object(), "setValue", {Value(static_cast<double>(state.progress))});
					}
				}
			}
			return;
		}
		if (!isServerList(list.listId) || !list.holder) {
			return;
		}
		const std::string prefix = list.rowPrefix(vm);
		for (int i = 0; i < list.visibleRows(vm); ++i) {
			const Value row = vm.getMember(Value(list.holder), prefix + std::to_string(i));
			const int index = list.firstIndex + i;
			bool favourite = false;
			const Server* listed = index < count ? serverAt(list.listId, index, &favourite) : nullptr;
			const bool server = listed != nullptr;
			if (isHeaderRow(list.listId, index)) {
				vm.setMember(row, "isClickable", Value(0));
			}
			const net::ServerEntry* e = server ? &listed->entry : nullptr;
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
			// The add icon on servers not yet favourites, the remove icon on
			// the favourites (images/list/favoriteServerAdd/Remove.png).
			bool isFavourite = favourite;
			for (const Server& f : m_favouriteServers) {
				isFavourite = isFavourite || (e && f.entry.ip == e->ip && f.gamePort == listed->gamePort);
			}
			visible("mcIconFavAdd", server && !isFavourite);
			visible("mcIconFavRemove", server && isFavourite);
			visible("mcIconInfo", server);
			visible("mcIconMod", server && !mod.empty() && mod != kModName);
			visible("mcIconPunkbuster", false); // no anti-cheat in the rewrite
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
		if (list.listId == "serverListLAN") {
			if (start) {
				startLanQuery();
			}
			const bool busy = finishLanQuery();
			show(vm, list);
			return Value(busy);
		}
		if (isInternetList(list.listId)) {
			if (start) {
				startServerQuery();
			}
			const bool busy = finishServerQuery();
			show(vm, list);
			return Value(busy);
		}
		if (list.listId == "awardProgress") {
			fillAwardProgress(list.args.empty() ? std::string() : list.args[0]);
			show(vm, list);
			return Value(false);
		}
		if (list.listId == "serverExtendedInfo") {
			// The server info popup's rules (0x10068150 / 0x10068080): the
			// server its arguments name ("ip gameport queryport").
			const std::string host = list.args.empty() ? std::string() : list.args[0];
			const int port = list.args.size() > 1 ? std::atoi(list.args[1].c_str()) : 0;
			m_serverInfoRows = serverInfoRows(host, port);
			show(vm, list);
			return Value(false);
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
		} else if (list.listId.compare(0, 9, "selectBox") == 0 && !list.args.empty() && list.args[0] == "botSkill") {
			// SwiffPlayer.dll's select box of the bot skills (0x100653f0 makes
			// 0x10061310): Easy, Normal, Hard, Normal chosen (+0x28 = 1).
			m_natives.setListRows("botSkill", ui::NativeRegistry::Rows {{m_natives.localize("HEADER_easy")},
					{m_natives.localize("HEADER_normal")}, {m_natives.localize("HEADER_hard")}});
			list.selectedIndex = 1;
			list.releasePending = true; // +0x18
		} else if (list.listId.compare(0, 9, "selectBox") == 0 && !list.args.empty() && list.args[0] == "displayModes") {
			// The display modes' box (0x10065360 fills with 0x10065250): the
			// mode in use (VideoSettings slot 0x80) chosen, +0x18 set.
			const auto setting = m_settings.find("VideoSettings.Resolution");
			const std::string current = setting != m_settings.end() ? setting->second.isString() ? setting->second.asString() : std::string()
			                                                        : std::string("1280x720@60Hz");
			const auto& modes = m_natives.listRows("displayModes");
			list.selectedIndex = -1;
			for (size_t i = 0; i < modes.size(); ++i) {
				if (!modes[i].empty() && modes[i][0] == current) {
					list.selectedIndex = static_cast<int>(i);
					break;
				}
			}
			list.releasePending = true;
		} else if (list.listId.compare(0, 9, "selectBox") == 0 && !list.args.empty() && list.args[0] == "gameModes") {
			// The game modes' select box (0x100690b0; filled by 0x10069160,
			// rows drawn by 0x100689a0): "gameModes <rows> <all>" lists the
			// mod's game modes (its list +0x10c, each mode once in the order
			// the level scan meets them: allGameModes),
			// after an "All" row when <all> is 1. A mode shows as its
			// TEXT_<id> when that has a translation, else its name in the mod
			// (ModInfo slot 0x48) unless "N/A", else its id. Chosen: "All"
			// with the row, else the server settings' mode (gpm_cq without
			// one), found before "sp1" and "gpm_sp" leave the list (so a
			// mode after a removed one is chosen one row late, as in the
			// original). +0x18 is set: the chosen row's rowRelease then
			// gives the box its text (NativeList::populate).
			const int all = list.args.size() > 2 ? std::max(0, std::atoi(list.args[2].c_str())) : 0;
			const std::vector<std::string> modes = selectBoxGameModes();
			ui::NativeRegistry::Rows rows;
			if (all > 0) {
				rows.push_back({m_natives.localize("HEADER_all")});
			}
			for (const std::string& mode : modes) {
				const std::string key = "TEXT_" + mode;
				std::string text = m_natives.localize(key);
				if (text == key) {
					const std::string name = gameModeName ? gameModeName(mode) : std::string("N/A");
					text = name == "N/A" ? mode : name;
				}
				rows.push_back({text});
			}
			m_natives.setListRows("gameModes", rows);
			list.selectedIndex = 0;
			if (all == 0) {
				const auto setting = m_settings.find("ServerSettings.GameMode");
				std::string current = setting != m_settings.end() && setting->second.isString() ? setting->second.asString() : std::string();
				const std::vector<std::string> scanned = allGameModes();
				const auto chosen = std::find(scanned.begin(), scanned.end(), current.empty() ? std::string("gpm_cq") : current);
				list.selectedIndex = chosen == scanned.end() ? -1 : static_cast<int>(chosen - scanned.begin());
			}
			list.releasePending = true;
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
		// The game modes' select box (0x10068d90): the mode's index after
		// the "All" row, its shown name and its id ("all" for that row).
		if (list.listId.compare(0, 9, "selectBox") == 0 && !list.args.empty() && list.args[0] == "gameModes") {
			const int all = list.args.size() > 2 ? std::max(0, std::atoi(list.args[2].c_str())) : 0;
			const std::vector<std::string> modes = selectBoxGameModes();
			auto object = vm.newObject();
			const int mode = index - all;
			vm.setMember(Value(object), "rowIndex", Value(mode));
			vm.setMember(Value(object), "rowValue", Value(list.rows()[static_cast<size_t>(index)].at(0)));
			vm.setMember(Value(object), "rowData",
				Value(mode < 0 ? std::string("all") : mode < static_cast<int>(modes.size()) ? modes[static_cast<size_t>(mode)] : std::string()));
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
		if (list.listId == "localProfiles" && m_profiles) {
			auto object = vm.newObject();
			const size_t row = static_cast<size_t>(index);
			vm.setMember(Value(object), "profileIndex", Value(row < m_localProfileIndices.size() ? m_localProfileIndices[row] : index));
			vm.setMember(Value(object), "profileName", Value(list.rows()[row].at(0)));
			return Value(object);
		}
		// The Mod page's rows (row class 0x10107d88, slot 13 0x10063e30):
		// the mod's index, which the page passes to Mod.selectMod.
		if (list.listId == "mods") {
			auto object = vm.newObject();
			vm.setMember(Value(object), "rowIndex", Value(index));
			return Value(object);
		}
		if (list.listId == "soldiers") {
			auto object = vm.newObject();
			vm.setMember(Value(object), "soldierName", Value(list.rows()[static_cast<size_t>(index)].at(0)));
			return Value(object);
		}
		const Server* listed = serverAt(list.listId, index);
		if (!listed) {
			return Value();
		}
		const Server& s = *listed;
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
		set("punkbuster", Value(0));
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

// SwiffHost slot 0x124 (0x004776a0), as Multiplay.joinServer (0x10043ed0)
// and Logic.joinCommandlineServer (0x1003c7a0) call it.
bool MenuBindings::joinServer(const std::string& host, int port)
{
	if (host.empty() || port <= 0 || port > 0xffff) {
		return false;
	}
	m_joinHost = host;
	m_joinPort = static_cast<uint16_t>(port);
	// The map the browser listed for it (the level to load).
	m_joinMap = {};
	int maxPlayers = 0;
	for (const auto* servers : {&m_servers, &m_lanServers, &m_favouriteServers}) {
		for (const Server& server : *servers) {
			if (server.entry.ip == host && server.gamePort == m_joinPort && m_joinMap.level.empty()) {
				m_joinMap = {field(server.entry, "mapname"), field(server.entry, "gametype"), fieldNumber(server.entry, "mapsize")};
				maxPlayers = fieldNumber(server.entry, "maxplayers");
			}
		}
	}
	setCurrentGame(m_joinMap, maxPlayers);
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
	return true;
}

// SwiffHost slot 0x128 (0x00477960), as Multiplay.createServer and
// Logic.startCommandlineServer (0x1003b130) call it.
bool MenuBindings::createServer()
{
	const GameSetup setup = gameSetup();
	if (setup.maps.empty()) {
		log("create server: no map chosen");
		return false;
	}
	log("create server: " + setup.maps.front().level + " " + setup.maps.front().gameMode + " "
		+ std::to_string(setup.maps.front().size) + ", " + std::to_string(setup.bots) + " bots");
	setCurrentGame(setup.maps.front(), setup.maxPlayers);
	if (onStartGame) {
		onStartGame(setup);
	}
	return true;
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
		return Value(joinServer(host, port));
	});
	n.setFunction(kMultiplay, "isGameSpyAvailable", [this](Interpreter&, const Args&) { return Value(m_account.connected); });
	n.setFunction(kMultiplay, "getNumServers", [this](Interpreter&, const Args&) {
		return Value(static_cast<double>(m_servers.size()));
	});
	// Asked while a search runs; a finished one's servers are taken then.
	n.setFunction(kMultiplay, "isBrowsing", [this](Interpreter&, const Args&) { return Value(finishServerQuery()); });
	n.setFunction(kMultiplay, "isServerBrowserBusy", [this](Interpreter&, const Args&) { return Value(finishServerQuery()); });
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
	n.setFunction(kMultiplay, "createServer", [this](Interpreter&, const Args&) { return Value(createServer()); });
	// The name of the server the info popup shows (its lists got the
	// address as arguments).
	n.setFunction(kServerInfo, "getServerName", [this](Interpreter&, const Args&) {
		for (const auto* servers : {&m_servers, &m_lanServers}) {
			for (const Server& server : *servers) {
				if (server.entry.ip == m_infoHost) {
					return Value(field(server.entry, "hostname"));
				}
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
	// What a setting is before the menu sets it (the game's defaults). The
	// host page's (the swf natives ask the exe's "GameServerSettings"
	// object, e.g. getRoundsPerMap 0x10056710 its slot 0x78) start as the
	// server settings the mod's Settings/ServerSettings.con gave (its
	// constructor 0x00499430, with the FileManager up: the file loaded,
	// then sv's serverName, maxPlayers (0x00431690: at least 16 ranked),
	// spawnTime and ticketRatio (0x00430f60 / 0x00430fa0: 15 and 100
	// ranked), roundsPerMap, autoBalanceTeam, teamRatioPercent and the
	// rest copied): the shipped file's are net::ServerSettings' defaults.
	// (Its other branch, "Battlefield-2142 Server", 16 players, gpm_cq,
	// spawn time 5 and 3 rounds, is for no FileManager: never in a game.)
	static const net::ServerSettings sv;
	static const std::map<std::string, Value> defaults = {
		{"ServerSettings.MaxPlayers", Value(sv.maxPlayers)},
		{"ServerSettings.RoundsPerMap", Value(sv.roundsPerMap)},
		{"ServerSettings.TicketRatio", Value(sv.ticketRatio)},
		{"ServerSettings.TeamRatio", Value(sv.teamRatioPercent)},
		{"ServerSettings.SpawnTime", Value(sv.spawnTime)},
		{"ServerSettings.FriendlyFire", Value(sv.soldierFriendlyFire > 0)},
		{"ServerSettings.AutoBalanceTeam", Value(sv.autoBalanceTeam)},
		{"ServerSettings.ServerName", Value(sv.serverName)},
		{"ServerSettings.GameMode", Value("gpm_sp")},
		{"GeneralSettings.MaxBots", Value(16)},
		{"GeneralSettings.BotSkill", Value(0.5)},
		{"GeneralSettings.UseBots", Value(true)},
		{"GeneralSettings.MaxBotsIncludeHumans", Value(true)},
		// The HUD page's values as the object keeps them (0x004960a0's
		// neighbours; SettingsObjects' defaults): alphas 0..255, the
		// crosshair "r g b a"; the natives below convert for the page.
		{"GeneralSettings.HUDTransparency", Value(255)},
		{"GeneralSettings.MinimapTransparency", Value(170)},
		{"GeneralSettings.MapIconTransparency", Value(255)},
		{"GeneralSettings.CrosshairColor", Value("255 255 255 255")},
		{"GeneralSettings.CameraShake", Value(1)},
		{"GeneralSettings.HelpPopUps", Value(1)},
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
		// The audio page's radios by name (0x004960a0's defaults; the
		// page sets "software", "hardware", "creative" and Low, Medium,
		// High, UltraHigh).
		{"AudioSettings.SoundQuality", Value("Medium")},
		{"AudioSettings.SoundDetail", Value(2)},
		{"AudioSettings.Provider", Value("software")},
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
			exportSettings(prefix);
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
	// 0x100108c0, 0x100109c0: the audio page's radios read the object's
	// provider and sound quality, as getProvider and getSoundQuality do.
	// The providers are lower case; files the rewrite wrote before it had
	// 0x004960a0's default say "Software", which no radio would match.
	n.setFunction(kAudioSettings, "getSelectedProvider", [this](Interpreter& vm, const Args&) {
		std::string provider = vm.toString(m_settings["AudioSettings.Provider"]);
		std::transform(provider.begin(), provider.end(), provider.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return Value(provider);
	});
	n.setFunction(kAudioSettings, "getSelectedSoundQuality", [this](Interpreter& vm, const Args&) {
		return Value(vm.toString(m_settings["AudioSettings.SoundQuality"]));
	});
	// 0x10010960: the sound detail the page no longer shows.
	n.setFunction(kAudioSettings, "getSelectedSoundDetail", [](Interpreter&, const Args&) { return Value("Remove Me"); });
	// The HUD page (SwiffPlayer 0x10030000..): its sliders are
	// transparencies 0..1, the object's values alphas 0..255 (get
	// 0x100326b0, 0x10032620, 0x10032590: 1 - alpha/255; set 0x100311b0,
	// 0x10031140, 0x100310d0: 255 - t*255).
	for (const char* name : {"HUDTransparency", "MinimapTransparency", "MapIconTransparency"}) {
		const std::string key = std::string("GeneralSettings.") + name;
		n.setFunction(kGeneralSettings, std::string("get") + name, [this, key](Interpreter& vm, const Args&) {
			return Value(1.0 - vm.toNumber(m_settings[key]) / 255.0);
		});
		n.setFunction(kGeneralSettings, std::string("set") + name, [this, key](Interpreter& vm, const Args& args) {
			m_settings[key] = Value(255.0 - vm.toNumber(arg(args, 0)) * 255.0);
			return Value();
		});
	}
	// 0x10032310: the crosshair as one number, alpha << 24 | red << 16 |
	// green << 8 | blue; 0x10030d80 sets it from (red, green, blue, alpha).
	n.setFunction(kGeneralSettings, "getCrosshairColor", [this](Interpreter& vm, const Args&) {
		int c[4] = {255, 255, 255, 255};
		std::sscanf(vm.toString(m_settings["GeneralSettings.CrosshairColor"]).c_str(), "%d %d %d %d", &c[0], &c[1], &c[2], &c[3]);
		const auto byte = [](int v) { return static_cast<uint32_t>(std::clamp(v, 0, 255)); };
		return Value(static_cast<double>(byte(c[3]) << 24 | byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2])));
	});
	n.setFunction(kGeneralSettings, "setCrosshairColor", [this](Interpreter& vm, const Args& args) {
		std::string color;
		for (size_t i = 0; i < 4; ++i) {
			color += (i ? " " : "") + std::to_string(static_cast<int>(std::lround(vm.toNumber(arg(args, i)))));
		}
		m_settings["GeneralSettings.CrosshairColor"] = Value(color);
		return Value();
	});
	// 0x10030180 / 0x100315d0: the camera shake radio is "no shake" (1
	// when the object's shake is off); 0x10030280 / 0x100316b0: help
	// pop-ups as they are.
	n.setFunction(kGeneralSettings, "getCameraShake", [this](Interpreter& vm, const Args&) {
		return Value(vm.toNumber(m_settings["GeneralSettings.CameraShake"]) != 0 ? 0.0 : 1.0);
	});
	n.setFunction(kGeneralSettings, "setCameraShake", [this](Interpreter&, const Args& args) {
		m_settings["GeneralSettings.CameraShake"] = Value(avm1::toBoolean(arg(args, 0)) ? 0.0 : 1.0);
		return Value();
	});
	n.setFunction(kGeneralSettings, "getHelpPopUps", [this](Interpreter& vm, const Args&) {
		return Value(vm.toNumber(m_settings["GeneralSettings.HelpPopUps"]) != 0 ? 1.0 : 0.0);
	});
	n.setFunction(kGeneralSettings, "setHelpPopUps", [this](Interpreter&, const Args& args) {
		m_settings["GeneralSettings.HelpPopUps"] = Value(avm1::toBoolean(arg(args, 0)) ? 1.0 : 0.0);
		return Value();
	});
	// 0x1005d2d0: the display mode in use, as the displayModes list writes it.
	n.setFunction(kVideoSettings, "getSelectedResolution", [this](Interpreter&, const Args&) {
		return m_settings.count("VideoSettings.Resolution") ? m_settings.at("VideoSettings.Resolution") : Value("");
	});
	// setGameModeIndex (0x10057870): the create-server page's choice by its
	// row in the game modes' box (sp1 and gpm_sp left out). The mode becomes
	// GameServerSettings' (slot 0x4c, then 0x18) and SwiffHost's filter mode
	// (slot 0x170, 0x00479e80: the available maps follow it).
	n.setFunction(kServerSettings, "setGameModeIndex", [this](Interpreter& vm, const Args& args) {
		const std::vector<std::string> modes = selectBoxGameModes();
		const int index = static_cast<int>(vm.toNumber(arg(args, 0)));
		if (index >= 0 && index < static_cast<int>(modes.size())) {
			const std::string& mode = modes[static_cast<size_t>(index)];
			m_settings["ServerSettings.GameMode"] = Value(mode);
			m_filterGameMode = mode;
			showMapLists();
		}
		return Value();
	});
	// The anti-cheat (PunkBuster) is not part of the rewrite: always off.
	for (const auto& [object, member] : std::vector<std::pair<const char*, const char*>> {
			 {kGeneralSettings, "getAllowPunkBuster"}, {kServerSettings, "getSvPunkBuster"}}) {
		n.setFunction(object, member, [](Interpreter&, const Args&) { return Value(false); });
	}
	for (const auto& [object, member] : std::vector<std::pair<const char*, const char*>> {
			 {kGeneralSettings, "setAllowPunkBuster"}, {kServerSettings, "setSvPunkBuster"}}) {
		n.setFunction(object, member, [](Interpreter&, const Args&) { return Value(); });
	}
	n.setFunction(kMessages, "getPunkBusterMessage", [](Interpreter&, const Args&) { return Value(""); });
	// The voice natives (dice.bf2.Sound.voip*) are Voip/VoipNatives'
	// (voip::bindVoipNatives, which main.cpp binds over these with the
	// voice chat and this object's AudioSettings). Without voice chat (a
	// menu built alone, as in the tests) voipInitialize fails, as
	// SwiffPlayer's (0x10059fa0) does without a voice manager, and the
	// others do nothing.
	n.setFunction("dice.bf2.Sound", "voipInitialize", [](Interpreter&, const Args&) { return Value(false); });
	for (const char* member : {"voipSetEnable", "voipStopTest", "voipStartTest", "voipSetBoost", "voipSetTransmitVolume",
			 "voipSetRecieveVolume", "voipSetThresholdVolume", "voipSetUsePushToTalk"}) {
		n.setFunction("dice.bf2.Sound", member, [](Interpreter&, const Args&) { return Value(); });
	}
	n.setFunction("dice.bf2.Sound", "voipGetEnable", [](Interpreter&, const Args&) { return Value(false); });
	for (const char* member : {"voipGetBoost", "voipGetTransmitVolume", "voipGetRecieveVolume", "voipGetThresholdVolume",
			 "voipGetUsePushToTalk", "voipIsBoostEnabled", "voipSendingVoice", "voipGetSendingVoiceVolume"}) {
		n.setFunction("dice.bf2.Sound", member, [](Interpreter&, const Args&) { return Value(); });
	}
	for (const auto& [key, value] : defaults) {
		m_settings.emplace(key, value);
	}
	m_appliedSettings = m_settings;
}

namespace {

// The pages' names that the settings objects spell differently.
constexpr std::pair<const char*, const char*> kSettingAliases[] = {
	{"AudioSettings.VoiceOverVolume", "AudioSettings.HelpVoiceVolume"},
	{"AudioSettings.NumberOfVoices", "AudioSettings.ForceNumberOfVoices"},
	{"VideoSettings.BloomEnabled", "VideoSettings.UseBloom"},
	{"GeneralSettings.MapIconTransparency", "GeneralSettings.MapIconAlphaTransparency"},
	{"GeneralSettings.HelpPopUps", "GeneralSettings.HelpPopups"},
};
// Every page setting goes to its object (the sound quality by its name,
// as AudioSettings.soundQuality keeps it).
bool syncedSetting(const std::string&)
{
	return true;
}
// An object's name that a page spells otherwise (kept under the page's).
bool aliasedObjectKey(const std::string& key)
{
	for (const auto& [from, to] : kSettingAliases) {
		if (key == to) {
			return true;
		}
	}
	return false;
}
std::string objectKey(const std::string& page)
{
	for (const auto& [from, to] : kSettingAliases) {
		if (page == from) {
			return to;
		}
	}
	return page;
}

} // namespace

void MenuBindings::setSettingsObjects(SettingsObjects* objects)
{
	m_objects = objects;
	if (!objects) {
		return;
	}
	// The objects' single values over the pages' defaults, keeping each
	// default's kind (a check box's boolean, a slider's number).
	for (auto& [key, value] : m_settings) {
		const std::string from = objectKey(key);
		if (!syncedSetting(key) || !objects->has(from)) {
			continue;
		}
		const std::string text = objects->get(from);
		if (text.empty() || (text.find(' ') != std::string::npos && key != "GeneralSettings.CrosshairColor")) {
			continue;
		}
		if (value.isBoolean()) {
			value = Value(std::atof(text.c_str()) != 0);
		} else if (value.isNumber()) {
			value = Value(std::atof(text.c_str()));
		} else {
			value = Value(text);
		}
		m_appliedSettings[key] = value;
	}
	refreshFavourites(); // the profile's General.con lists
	// The ones the pages had no default for.
	for (const auto& [key, text] : objects->values()) {
		if (!m_settings.count(key) && !aliasedObjectKey(key) && !text.empty() && text.find(' ') == std::string::npos &&
			syncedSetting(key)) {
			char* end = nullptr;
			const double number = std::strtod(text.c_str(), &end);
			const Value value = *end == 0 ? Value(number) : Value(text);
			m_settings[key] = value;
			m_appliedSettings[key] = value;
		}
	}
}

// apply(): the page's values to the settings objects, and their files saved.
void MenuBindings::exportSettings(const std::string& prefix)
{
	if (!m_objects) {
		return;
	}
	for (const auto& [key, v] : m_appliedSettings) {
		if (key.compare(0, prefix.size(), prefix) != 0 || !syncedSetting(key)) {
			continue;
		}
		const std::string to = objectKey(key);
		if (!m_objects->has(to)) {
			continue;
		}
		if (v.isBoolean()) {
			m_objects->set(to, v.asBoolean() ? "1" : "0");
		} else if (v.isNumber()) {
			m_objects->set(to, avm1::numberToString(v.asNumber()));
		} else if (v.isString()) {
			m_objects->set(to, v.asString());
		}
	}
	m_objects->save();
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
	// The page's maps by number (0x100173a0 indexes the table that
	// 0x100f7b50 fills): on foot, land vehicles, aircraft, helicopters,
	// ships, the game's map and the player inputs common to all.
	static const char* kMaps[] = {input::ControlMaps::kInfantry, input::ControlMaps::kLand, input::ControlMaps::kAir,
		input::ControlMaps::kHelicopter, input::ControlMaps::kSea, input::ControlMaps::kGame, input::ControlMaps::kPlayer};
	const auto mapName = [this] { return std::string(kMaps[std::clamp(m_controlMap, 0, 6)]); };
	n.setFunction(kControlSettings, "setActiveControlMap", [this](Interpreter& vm, const Args& args) {
		m_controlMap = static_cast<int>(vm.toNumber(arg(args, 0)));
		return Value();
	});
	n.setFunction(kControlSettings, "getActiveControlMap", [this](Interpreter&, const Args&) { return Value(m_controlMap); });
	// getMappedString(keyId, secondary, negative): the key on the input
	// (keyId: the engine's c_PI number) in that slot and side, as shown:
	// 0x10018050 localizes the "CONTROLLER_..." key the game gives
	// (0x00498750: "CONTROLLER_IDKey_W", "CONTROLLER_IDFMouse_IDButton_0",
	// "CONTROLLER_IDFGameController_IDButton_3"), nothing for none.
	n.setFunction(kControlSettings, "getMappedString", [this, mapName](Interpreter& vm, const Args& args) {
		if (!m_controls) {
			return Value("");
		}
		// The game map's numbers are its c_GI inputs' (0x10014ef0's game
		// table: 55 the scoreboard, 1 the console), the others c_PI.
		const std::string input = input::inputNamed(static_cast<int>(vm.toNumber(arg(args, 0))), mapName() != input::ControlMaps::kGame);
		const input::Binding* b =
			m_controls->binding(mapName(), input, avm1::toBoolean(arg(args, 1)) ? 1 : 0, avm1::toBoolean(arg(args, 2)) ? -1.0f : 1.0f);
		if (!b) {
			return Value("");
		}
		const std::string key = input::ControlMaps::displayKey(*b);
		const std::string text = m_natives.localize(key);
		if ((!text.empty() && text != key) || key.compare(0, 7, "Falcon_") == 0) {
			return Value(text);
		}
		// No localization: "IDKey_W" as "W", "IDButton_0" as "Mouse 1".
		if (b->control.compare(0, 9, "IDButton_") == 0 && b->device == "IDFMouse") {
			return Value("Mouse " + std::to_string(std::atoi(b->control.c_str() + 9) + 1));
		}
		if (input::GameControllers::slotNamed(b->device) >= 0) {
			return Value((b->kind == input::Binding::Kind::Axis ? "Joy Axis " : "Joy Button ") +
				std::to_string(std::atoi(b->control.c_str() + b->control.find('_') + 1)));
		}
		return Value(b->control.compare(0, 6, "IDKey_") == 0 ? b->control.substr(6) : b->control);
	});
	// mapInput(keyId, secondary, negative, superceed) (0x100199f0): binds
	// the input pressed last there (a key, a mouse button, or a game
	// controller's button or axis: an axis goes on as an axis mapping,
	// inverted when it was pushed the other way from the side being
	// bound; superceed is the page's flag for the common player map, set
	// active before) and answers ControlSettings slot 0x30's error code: 0 bound,
	// 1 when the key is another input's in the same map or the common
	// player map (the page then asks, showing getLastDuplicateInput, and
	// forceSetLastInput binds it anyway, taking it off the other).
	n.setFunction(kControlSettings, "mapInput", [this, mapName](Interpreter& vm, const Args& args) {
		// The game map's numbers are its c_GI inputs' (0x10014ef0's game
		// table: 55 the scoreboard, 1 the console), the others c_PI.
		const std::string input = input::inputNamed(static_cast<int>(vm.toNumber(arg(args, 0))), mapName() != input::ControlMaps::kGame);
		if (!m_controls || input.empty() || m_lastInput.empty()) {
			return Value(2);
		}
		const float side = avm1::toBoolean(arg(args, 2)) ? -1.0f : 1.0f;
		const bool axis = m_lastInput.compare(0, 7, "IDAxis_") == 0;
		PendingInput pending;
		pending.map = mapName();
		pending.input = input;
		pending.device = m_lastDevice.empty() ? std::string("IDFKeyboard") : m_lastDevice;
		pending.control = m_lastInput;
		pending.slot = avm1::toBoolean(arg(args, 1)) ? 1 : 0;
		pending.sign = axis ? side * m_lastDirection : side;
		// The key's other input, if any (not the same input and side).
		for (const char* other : {pending.map.c_str(), input::ControlMaps::kPlayer}) {
			const input::ControlMaps::Map* map = m_controls->find(other);
			if (!map || (other != pending.map && pending.map == input::ControlMaps::kGame)) {
				continue;
			}
			for (const auto& [name, bindings] : *map) {
				for (const input::Binding& b : bindings) {
					if (b.device == pending.device && b.control == pending.control && pending.duplicateInput.empty()
						&& !(name == input && other == pending.map && (axis || b.sign == side))) {
						pending.duplicateMap = other;
						pending.duplicateInput = name;
						pending.duplicateSign = b.sign;
					}
				}
			}
		}
		if (!pending.duplicateInput.empty()) {
			m_pendingInput = pending;
			return Value(1);
		}
		m_pendingInput = pending;
		m_pendingInput->duplicateInput.clear();
		bindPendingInput();
		return Value(0);
	});
	// 0x1001aa80: the duplicate's action as the page names it, the
	// HEADER_<action> text of 0x10014ef0's table (0x10016250), "" when
	// the table has none.
	n.setFunction(kControlSettings, "getLastDuplicateInput", [this](Interpreter&, const Args&) {
		if (!m_pendingInput || m_pendingInput->duplicateInput.empty()) {
			return Value("");
		}
		const std::string header = controlHeader(m_pendingInput->duplicateMap, m_pendingInput->duplicateInput,
			m_pendingInput->duplicateSign < 0);
		return Value(header.empty() ? std::string() : m_natives.localize(header));
	});
	n.setFunction(kControlSettings, "forceSetLastInput", [this](Interpreter&, const Args&) {
		bindPendingInput();
		return Value();
	});
}

// A value set outside the pages (the voice natives write the live
// AudioSettings object, SwiffPlayer 0x1005a240 and the like): the page's
// value and the applied one, written down at once.
void MenuBindings::storeSetting(const std::string& name, const Value& value)
{
	m_settings[name] = value;
	m_appliedSettings[name] = value;
	saveSettings();
	const size_t dot = name.find('.');
	if (dot != std::string::npos) {
		exportSettings(name.substr(0, dot + 1));
	}
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

std::string MenuBindings::hoursMinutes(std::string format, int minutes)
{
	const auto replace = [&format](const char* marker, int value) {
		if (const size_t at = format.find(marker); at != std::string::npos) {
			char digits[16];
			std::snprintf(digits, sizeof(digits), "%02d", value);
			format.replace(at, 4, digits);
		}
	};
	replace("#HH#", minutes / 60);
	replace("#MM#", minutes % 60);
	return format;
}

// The mod's game modes (ModInfo +0x10c) in the order the level scan meets
// them. The scan (0x004643d0) lists <mod>/Levels/ with the file system's
// directory listing (slot 0x34; FindFirstFileW / FindNextFileW in turn,
// 0x0069f530, which on NTFS gives the names in upper-cased order) and reads
// each level's Info/<level>.desc (0x00462750): every <mode type="...">
// under <modes> in the file's order goes to the end of the list unless it
// is there already (compared without regard to case, 0x0065aed0). A level
// whose .desc can't be read adds nothing in the original; here it adds the
// modes its server.zip has, in name order.
std::vector<std::string> MenuBindings::allGameModes() const
{
	const auto folded = [](std::string text, int (*fold)(int)) {
		for (char& c : text) {
			c = static_cast<char>(fold(static_cast<unsigned char>(c)));
		}
		return text;
	};
	const auto upperText = [&](const std::string& text) { return folded(text, ::toupper); };
	const auto lowerText = [&](const std::string& text) { return folded(text, ::tolower); };
	std::vector<std::string> levels;
	for (const MenuMap& map : m_maps) {
		if (std::find(levels.begin(), levels.end(), map.level) == levels.end()) {
			levels.push_back(map.level);
		}
	}
	std::stable_sort(levels.begin(), levels.end(), [&upperText](const std::string& a, const std::string& b) {
		return upperText(a) < upperText(b);
	});
	std::vector<std::string> modes;
	const auto add = [&modes, &lowerText](const std::string& mode) {
		if (mode.empty()) {
			return;
		}
		const std::string lower = lowerText(mode);
		if (std::none_of(modes.begin(), modes.end(), [&](const std::string& m) { return lowerText(m) == lower; })) {
			modes.push_back(mode);
		}
	};
	for (const std::string& level : levels) {
		if (const LevelText* text = levelText(level)) {
			for (const std::string& mode : text->modes) {
				add(mode);
			}
			continue;
		}
		for (const MenuMap& map : m_maps) { // sorted by level, then mode
			if (map.level == level) {
				add(map.gameMode);
			}
		}
	}
	return modes;
}

// The modes the create-server page offers: the mod's without the first
// "sp1" and the first "gpm_sp" (0x10069160's fill, and setGameModeIndex
// 0x10057870, take them out the same way).
std::vector<std::string> MenuBindings::selectBoxGameModes() const
{
	std::vector<std::string> modes = allGameModes();
	for (const char* hidden : {"sp1", "gpm_sp"}) {
		if (const auto it = std::find(modes.begin(), modes.end(), hidden); it != modes.end()) {
			modes.erase(it);
		}
	}
	return modes;
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
	setup.maxPlayers = static_cast<int>(setting("ServerSettings.MaxPlayers", 64));
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
	setup.roundsPerMap = std::max(1, static_cast<int>(setting("ServerSettings.RoundsPerMap", 3)));
	return setup;
}

// --- SwiffHost and MenuTeamManager (the menu scripts' console objects)

namespace {

std::string lowerText(std::string text)
{
	for (char& c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

} // namespace

std::string MenuHost::gameModeName(const std::string& id) const
{
	const auto it = gameModes.find(lowerText(id));
	return it == gameModes.end() ? std::string("N/A") : it->second;
}

void registerMenuHostConsole(Console& console, MenuHost& host)
{
	const auto add = [&console](const std::string& name, int minArgs, int maxArgs,
						 std::function<std::string(const std::vector<std::string>&)> run) {
		ConsoleMethod method;
		method.name = name;
		method.access = ConsoleAccess::Anyone;
		method.minArgs = minArgs;
		method.maxArgs = maxArgs;
		method.invoke = std::move(run);
		console.registerMethod(std::move(method));
	};
	const auto integer = [](const std::string& text) { return std::atoi(text.c_str()); };
	MenuHost* h = &host;
	// 0x0047a0c0: {text, size} kept as written (localized when drawn).
	add("SwiffHost.addCredit", 2, 2, [h, integer](const std::vector<std::string>& args) {
		if (h->scroller) {
			h->scroller->addCredit(args[0], integer(args[1]));
		}
		return std::string();
	});
	add("SwiffHost.setScrollFadeSpeed", 1, 1, [h, integer](const std::vector<std::string>& args) {
		if (h->scroller) {
			h->scroller->setFadeSpeed(integer(args[0]));
		}
		return std::string();
	});
	add("SwiffHost.setScrollSpeed", 1, 1, [h](const std::vector<std::string>& args) {
		if (h->scroller) {
			h->scroller->setSpeed(static_cast<float>(std::atof(args[0].c_str())));
		}
		return std::string();
	});
	add("SwiffHost.enableScroller", 1, 1, [h, integer](const std::vector<std::string>& args) {
		if (h->scroller) {
			h->scroller->enable(integer(args[0]) != 0 || lowerText(args[0]) == "true");
		}
		return std::string();
	});
	add("SwiffHost.setScrollerRect", 4, 4, [h, integer](const std::vector<std::string>& args) {
		if (h->scroller) {
			h->scroller->setRect(integer(args[0]), integer(args[1]), integer(args[2]), integer(args[3]));
		}
		return std::string();
	});
	add("SwiffHost.addGameMode", 2, 2, [h](const std::vector<std::string>& args) {
		h->gameModes[lowerText(args[0])] = args[1];
		return std::string();
	});
	add("SwiffHost.addAward", 4, 4, [h, integer](const std::vector<std::string>& args) {
		MenuHost::Award award;
		award.id = integer(args[1]);
		award.level = integer(args[2]);
		award.value = integer(args[3]);
		award.kind = award.id < 200 ? 1 : award.id < 300 ? 0 : award.id < 400 ? 2 : 3;
		const std::string suffix = std::to_string(award.id) + (award.kind == 1 ? "_" + std::to_string(award.level) : "");
		award.nameKey = "AWARD_NAME_" + suffix;
		award.descriptionKey = "AWARD_DESCRIPTION_" + suffix;
		h->awards.push_back(award);
		return std::string();
	});
	add("SwiffHost.loadImage", 1, 1, [h](const std::vector<std::string>& args) {
		return std::string(h->loadImage && h->loadImage(args[0]) ? "1" : "0");
	});
	add("SwiffHost.joinServer", 3, 3, [h, integer](const std::vector<std::string>& args) {
		return std::string(h->joinServer && h->joinServer(args[0], integer(args[1]), integer(args[2])) ? "1" : "0");
	});
	add("SwiffHost.disconnectGame", 0, 0, [h](const std::vector<std::string>&) {
		return std::string(h->disconnectGame && h->disconnectGame() ? "1" : "0");
	});
	add("SwiffHost.setLatestGameVersion", 1, 1, [h](const std::vector<std::string>& args) {
		h->latestGameVersion = args[0];
		return std::string();
	});
	add("MenuTeamManager.addTeam", 2, 2, [h](const std::vector<std::string>& args) {
		h->teams.push_back({args[0], args[1], {}});
		return std::string();
	});
	add("MenuTeamManager.addKit", 1, 1, [h](const std::vector<std::string>& args) {
		if (!h->teams.empty()) {
			h->teams.back().kits.push_back(args[0]);
		}
		return std::string();
	});
	add("MenuTeamManager.addWeapon", 4, 4, [h, integer](const std::vector<std::string>& args) {
		h->weapons[lowerText(args[0])] = {integer(args[1]), integer(args[2]), integer(args[3])};
		return std::string();
	});
}

} // namespace dice::hfe::menu
