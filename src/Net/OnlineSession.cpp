#include "OnlineSession.h"

#include "OnlineConfig.h"
#include "PlayerAuth.h"

#include <chrono>

namespace dice::hfe::net {

namespace {

std::string feslFailure(const std::string& step, const std::optional<FeslMessage>& reply, long* code)
{
	if (!reply) {
		return step + ": no reply";
	}
	if (code) {
		*code = reply->number("errorCode");
	}
	return step + ": " + feslErrorName(reply->number("errorCode")) + " (" + reply->get("errorCode") + ")";
}

} // namespace

OnlineSession::~OnlineSession() { logout(); }

void OnlineSession::log(const std::string& text) const
{
	if (onLog) {
		onLog(text);
	}
}

std::optional<FeslAccount> OnlineSession::loginAccount(const std::string& name, const std::string& password, std::string* error,
	long* feslError)
{
	logout();
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto fail = [&](const std::string& why) -> std::optional<FeslAccount> {
		if (error) {
			*error = why;
		}
		return std::nullopt;
	};
	m_account = {};
	m_identity = {};
	const OnlineEndpoints e = m_endpoints ? *m_endpoints : onlineConfig().endpoints;
	std::string why;
	if (!m_fesl.connect(e.fesl.host, e.fesl.port, 10, &why, e.feslTls)) {
		return fail("FESL " + e.fesl.text() + ": " + why);
	}
	log("fesl: connected to " + e.fesl.text() + (e.feslTls ? " (TLS)" : " (plain TCP)"));
	auto reply = m_fesl.hello();
	if (!reply || reply->isError()) {
		return fail(feslFailure("hello", reply, feslError));
	}
	reply = m_fesl.login(name, password);
	if (!reply || reply->isError()) {
		return fail(feslFailure("login", reply, feslError));
	}
	m_account.displayName = reply->text("displayName");
	m_account.lkey = reply->text("lkey");
	m_account.userId = reply->number("userId");
	m_account.profileId = reply->number("profileId");
	reply = m_fesl.subAccounts();
	if (!reply || reply->isError()) {
		return fail(feslFailure("sub-accounts", reply, feslError));
	}
	m_account.soldiers = reply->list("subAccounts");
	log("fesl: logged in as " + m_account.displayName + " (user " + std::to_string(m_account.userId) + ", "
		+ std::to_string(m_account.soldiers.size()) + " soldiers)");
	return m_account;
}

bool OnlineSession::loginSoldier(const std::string& soldier, std::string* error, long* feslError)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto fail = [&](const std::string& why) {
		if (error) {
			*error = why;
		}
		return false;
	};
	if (!m_fesl.connected()) {
		return fail("not logged in to the account service");
	}
	const std::string nick = soldier.empty() && !m_account.soldiers.empty() ? m_account.soldiers.front() : soldier;
	if (nick.empty()) {
		return fail("the account has no soldiers");
	}
	auto reply = m_fesl.loginSubAccount(nick);
	if (!reply || reply->isError()) {
		return fail(feslFailure("soldier login", reply, feslError));
	}
	OnlineIdentity identity;
	identity.soldier = nick;
	identity.profileId = reply->number("profileId");
	identity.nameChecksum = nameChecksum(nick); // 0x004a1d70 case 0xb -> 0x005795f0
	identity.cdKey = m_cdKey ? *m_cdKey : onlineConfig().credentials.cdKey;
	log("fesl: soldier " + nick + " (profile " + std::to_string(identity.profileId) + ")");
	reply = m_fesl.gameSpyPreAuth();
	if (!reply || reply->isError()) {
		return fail(feslFailure("GameSpy pre-authentication", reply, feslError));
	}
	const std::string ticket = reply->text("ticket");
	const std::string challenge = reply->text("challenge");
	const OnlineEndpoints e = m_endpoints ? *m_endpoints : onlineConfig().endpoints;
	std::string why;
	if (!m_gp.connect(e.gpcm.host, e.gpcm.port, 10, &why)) {
		return fail("GP " + e.gpcm.text() + ": " + why);
	}
	const auto gp = m_gp.loginPreAuthenticated(ticket, challenge, &why);
	if (!gp) {
		return fail("GP login: " + why);
	}
	identity.loginTicket = gp->loginTicket;
	log("gpcm: logged in as " + gp->uniquenick + " (profile " + std::to_string(gp->profileId) + ", login ticket "
		+ std::to_string(gp->loginTicket.size()) + " characters)");
	m_identity = identity;
	return true;
}

bool OnlineSession::login(const std::string& name, const std::string& password, const std::string& soldier, std::string* error)
{
	return loginAccount(name, password, error) && loginSoldier(soldier, error);
}

void OnlineSession::keepAlive()
{
	if (m_running.exchange(true)) {
		return;
	}
	m_keepAlive = std::thread([this] {
		while (m_running) {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_fesl.pump(0);
				m_gp.pump(0);
			}
			for (int i = 0; i < 10 && m_running; ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	});
}

void OnlineSession::logout()
{
	m_running = false;
	if (m_keepAlive.joinable()) {
		m_keepAlive.join();
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_gp.connected()) {
		m_gp.logout();
	}
	if (m_fesl.connected()) {
		m_fesl.goodbye();
	}
	m_identity = {};
}

OnlineIdentity OnlineSession::identity() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_identity;
}

} // namespace dice::hfe::net
