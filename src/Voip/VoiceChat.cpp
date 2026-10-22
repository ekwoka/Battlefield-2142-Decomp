#include "VoiceChat.h"

#include "Net/GameProtocol.h"

#include <chrono>
#include "VoipServerConnection.h"

namespace dice::hfe::voip {

VoiceChat::VoiceChat(std::unique_ptr<VoiceDevice> device) : m_device(std::move(device))
{
	m_client.setDevice(m_device.get());
	m_client.setSettings(m_settings);
}

VoiceChat::~VoiceChat()
{
	runInBackground(false);
	m_client.stop();
	m_client.setDevice(nullptr);
}

void VoiceChat::runInBackground(bool on)
{
	if (on == m_thread.joinable()) {
		return;
	}
	if (!on) {
		m_stop = true;
		m_thread.join();
		return;
	}
	m_stop = false;
	m_thread = std::thread([this] {
		while (!m_stop) {
			update(clock());
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	});
}

double VoiceChat::clock() const
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void VoiceChat::setDevice(std::unique_ptr<VoiceDevice> device)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_client.setDevice(nullptr);
	m_device = std::move(device);
	m_client.setDevice(m_device.get());
}

bool VoiceChat::openDevice()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (!m_device) {
		setDevice(openVoiceDevice());
	}
	return m_device != nullptr;
}

void VoiceChat::setServer(const ServerVoipSettings& settings, const std::string& gameServerHost)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_server = settings;
	m_gameServerHost = gameServerHost;
	m_haveServer = true;
	tryStart();
}

bool VoiceChat::setServerSettingsBlock(const std::string& block, const std::string& gameServerHost)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (block == m_lastBlock) {
		return m_haveServer;
	}
	m_lastBlock = block;
	ServerVoipSettings settings;
	if (!parseServerVoipSettings(block, settings)) {
		return false;
	}
	setServer(settings, gameServerHost);
	return true;
}

bool VoiceChat::handleEvent(const net::GameEvent& event, int localPlayer)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	switch (event.type) {
	case net::kEvVoipSession:
		// 0x00512250 / 0x00512320: our session id.
		m_session = static_cast<uint16_t>(event.u(0));
		m_client.setSession(m_session);
		tryStart();
		return true;
	case net::kEvVoipPlayerMute:
		// 0x00510e10 / 0x00510f50: (mute, the muted player, the player muting);
		// the voice of a player we muted is dropped here.
		if (static_cast<int>(event.u(2)) == localPlayer) {
			m_client.setMuted(static_cast<int>(event.u(1)), event.u(0) != 0);
		}
		return true;
	case net::kEvVoipOnOff:
		// 0x00510c20 / 0x00510d30: a player's voice chat on or off (the HUD's icons).
		m_voipOn[static_cast<int>(event.u(0))] = event.u(1) != 0;
		return true;
	default:
		return false;
	}
}

void VoiceChat::tryStart()
{
	if (m_client.running() || !m_haveServer || m_session == 0 || !m_server.enabled || !m_settings.enabled) {
		return;
	}
	// 0x0043c630: address 1 is the game server's own (127.0.0.1 when we host).
	std::string host = m_gameServerHost.empty() ? "127.0.0.1" : m_gameServerHost;
	net::UdpServerSocket::Address server;
	if (m_server.address != 1 && m_server.address != 0) {
		server.ip = m_server.address;
		server.port = static_cast<uint16_t>((m_server.serverPort >> 8) | (m_server.serverPort << 8));
	} else if (!resolveAddress(host, m_server.serverPort, server)) {
		return;
	}
	openDevice();
	// sv.voipBFClientPort, or any port when it is taken (two games on one machine).
	if (!m_client.start(server, m_server.clientPort, m_server.quality) && !m_client.start(server, 0, m_server.quality)) {
		return;
	}
	m_client.setSession(m_session);
	applyChannels();
}

void VoiceChat::leave()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_client.stop();
	m_haveServer = false;
	m_lastBlock.clear();
	m_session = 0;
	m_voipOn.clear();
}

void VoiceChat::setRole(bool commander, bool squadLeader, int squad, const std::vector<int>& selectedSquads)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_commander = commander;
	m_squadLeader = squadLeader;
	m_squad = squad;
	m_selectedSquads = selectedSquads;
	applyChannels();
}

void VoiceChat::setKeys(bool pushToTalk, bool leaderChannel)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_client.setPushToTalk(pushToTalk);
	m_leaderKey = leaderChannel;
	applyChannels();
}

void VoiceChat::applyChannels()
{
	// 0x0043c8d0: only the commander and squad members talk.
	m_client.setTransmitAllowed(m_commander || (m_squad >= 1 && m_squad <= 9));
	m_client.setChannels(selectChannels(m_commander, m_squadLeader, m_leaderKey, m_selectedSquads));
}

void VoiceChat::update(double now)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_client.update(now);
}

bool VoiceChat::playerVoipOn(int player) const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	const auto it = m_voipOn.find(player);
	return it != m_voipOn.end() && it->second;
}

void VoiceChat::setSettings(const VoiceSettings& settings)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	const bool wasEnabled = m_settings.enabled;
	m_settings = settings;
	m_client.setSettings(settings);
	if (!settings.enabled && wasEnabled) {
		m_client.stop();
	} else if (settings.enabled && !wasEnabled) {
		tryStart();
	}
}

void VoiceChat::startTest()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	openDevice();
	if (!m_client.running()) {
		m_client.setQuality(m_haveServer ? m_server.quality : 3);
	}
	m_client.setLoopbackTest(true);
}

void VoiceChat::stopTest()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_client.setLoopbackTest(false);
}

bool VoiceChat::hasDevice() const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	return m_device != nullptr;
}

bool VoiceChat::active() const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	return m_client.running();
}

VoiceSettings VoiceChat::settings() const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	return m_settings;
}

bool VoiceChat::testing() const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	return m_client.loopbackTest();
}

bool VoiceChat::sendingVoice() const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	return m_client.transmitting();
}

float VoiceChat::sendingVoiceVolume() const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	return m_client.captureLevel();
}

} // namespace dice::hfe::voip
