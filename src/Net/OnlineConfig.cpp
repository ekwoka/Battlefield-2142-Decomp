#include "OnlineConfig.h"

#include "Fesl.h"
#include "ServerBrowser.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace dice::hfe::net {

namespace {

std::mutex g_mutex;
OnlineConfig g_config;

std::string trim(const std::string& s)
{
	const size_t begin = s.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) {
		return {};
	}
	const size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(begin, end - begin + 1);
}

std::string env(const char* name)
{
	const char* value = std::getenv(name);
	return value ? value : "";
}

void setEndpoint(ServiceEndpoint& endpoint, const std::string& value)
{
	const size_t colon = value.rfind(':');
	if (colon == std::string::npos) {
		endpoint.host = value;
	} else {
		endpoint.host = value.substr(0, colon);
		endpoint.port = static_cast<uint16_t>(std::atoi(value.c_str() + colon + 1));
	}
}

bool parseIpv4(const std::string& host, uint8_t (&octets)[4])
{
	int a, b, c, d;
	char tail;
	if (std::sscanf(host.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) {
		return false;
	}
	for (int v : {a, b, c, d}) {
		if (v < 0 || v > 255) {
			return false;
		}
	}
	octets[0] = static_cast<uint8_t>(a);
	octets[1] = static_cast<uint8_t>(b);
	octets[2] = static_cast<uint8_t>(c);
	octets[3] = static_cast<uint8_t>(d);
	return true;
}

} // namespace

OnlineEndpoints liveEndpoints()
{
	// RendDX9.dll's patches (openspy/bf2142_rendhook Source.cpp): gpcm at
	// 0x00a08b80 -> gpcm.openspy.net, the browser's "%s.ms%d.openspy.net"
	// at 0x009a0208, stella.prod.openspy.net at 0x009645c4; FESL_impl.cpp:
	// fesl.openspy.net:18301 over TLS.
	OnlineEndpoints e;
	e.fesl = {kFeslHost, kFeslPort};
	e.feslTls = true;
	e.gpcm = {"gpcm.openspy.net", 29900};
	e.master = {masterServerHost(kGameName, "openspy.net"), kMasterPort};
	e.stella = {"stella.prod.openspy.net", 80};
	e.cdkey = {"cdkey.openspy.net", 29910};
	return e;
}

OnlineEndpoints localEndpoints(const std::string& host)
{
	OnlineEndpoints e;
	e.fesl = {host, kFeslGamePort};
	e.feslTls = false;
	e.gpcm = {host, 29900};
	e.master = {host, kMasterPort};
	e.stella = {host, 18080};
	e.cdkey = {host, 29910};
	return e;
}

void applyOnlineConfigText(OnlineConfig& config, const std::string& text)
{
	std::istringstream in(text);
	std::string line;
	std::string services, fesl, gpcm, master, stella;
	while (std::getline(in, line)) {
		const size_t hash = line.find('#');
		if (hash != std::string::npos) {
			line.resize(hash);
		}
		const size_t equals = line.find('=');
		if (equals == std::string::npos) {
			continue;
		}
		const std::string key = trim(line.substr(0, equals));
		const std::string value = trim(line.substr(equals + 1));
		if (key == "account") {
			config.credentials.account = value;
		} else if (key == "password") {
			config.credentials.password = value;
		} else if (key == "soldier") {
			config.credentials.soldier = value;
		} else if (key == "cdkey") {
			config.credentials.cdKey = value;
		} else if (key == "services") {
			services = value;
		} else if (key == "fesl") {
			fesl = value;
		} else if (key == "gpcm") {
			gpcm = value;
		} else if (key == "master") {
			master = value;
		} else if (key == "stella") {
			stella = value;
		}
	}
	// Host overrides only change the local services: online, the hosts
	// are OpenSpy's.
	if (config.online) {
		return;
	}
	if (!services.empty()) {
		config.endpoints = localEndpoints(services);
	}
	if (!fesl.empty()) {
		setEndpoint(config.endpoints.fesl, fesl);
	}
	if (!gpcm.empty()) {
		setEndpoint(config.endpoints.gpcm, gpcm);
	}
	if (!master.empty()) {
		setEndpoint(config.endpoints.master, master);
	}
	if (!stella.empty()) {
		setEndpoint(config.endpoints.stella, stella);
	}
}

OnlineConfig loadOnlineConfig(bool online)
{
	OnlineConfig config;
	config.online = online;
	config.endpoints = online ? liveEndpoints() : localEndpoints();
	std::string path = env("BF2142_ONLINE_CONFIG");
	if (path.empty()) {
		path = "bf2142-online.cfg";
		if (!std::ifstream(path)) {
			const std::string home = env("HOME");
			path = home.empty() ? std::string() : home + "/.config/bf2142/online.cfg";
		}
	}
	if (!path.empty()) {
		if (std::ifstream file(path); file) {
			std::stringstream text;
			text << file.rdbuf();
			applyOnlineConfigText(config, text.str());
			config.file = path;
		}
	}
	if (const std::string v = env("BF2142_ACCOUNT"); !v.empty()) {
		config.credentials.account = v;
	}
	if (const std::string v = env("BF2142_PASSWORD"); !v.empty()) {
		config.credentials.password = v;
	}
	if (const std::string v = env("BF2142_SOLDIER"); !v.empty()) {
		config.credentials.soldier = v;
	}
	if (const std::string v = env("BF2142_CDKEY"); !v.empty()) {
		config.credentials.cdKey = v;
	}
	return config;
}

const OnlineConfig& onlineConfig()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_config;
}

void setOnlineConfig(const OnlineConfig& config)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_config = config;
}

bool hostAllowed(const std::string& host)
{
	if (host == "localhost") {
		return true;
	}
	uint8_t o[4];
	if (parseIpv4(host, o)) {
		const bool local = o[0] == 127 || o[0] == 10 || (o[0] == 192 && o[1] == 168)
			|| (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 169 && o[1] == 254)
			|| (o[0] == 255 && o[1] == 255 && o[2] == 255 && o[3] == 255);
		if (local) {
			return true;
		}
	}
	return onlineConfig().online;
}

std::string hostRefusal(const std::string& host)
{
	return host + " is not on this machine or its network; live servers need --online";
}

} // namespace dice::hfe::net
