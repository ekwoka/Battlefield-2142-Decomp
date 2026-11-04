// Where the online services are and who logs in to them, and the switch
// that keeps everything on this machine until the user turns it on.
//
// Live play (OpenSpy, the OpenSpy patch's hosts: RendDX9.dll redirects
// every GameSpy host to *.openspy.net and FESL to fesl.openspy.net:18301
// over TLS) is used only with `online` set, which only `--online` on the
// command line does. Without it every service is 127.0.0.1 (the local
// emulators, Net/OpenSpyEmulator.h: `bf2142 --openspy-emulator`), FESL
// speaks plain TCP there, and hostAllowed() refuses any public address,
// so no command can reach a real host by accident.
//
// The credentials (never in the repository) come from, in order:
//   - the environment: BF2142_ACCOUNT, BF2142_PASSWORD, BF2142_SOLDIER,
//     BF2142_CDKEY;
//   - a key=value file (git-ignored; '#' starts a comment): the path in
//     BF2142_ONLINE_CONFIG, else ./bf2142-online.cfg, else
//     ~/.config/bf2142/online.cfg, with the keys account, password,
//     soldier, cdkey, and optionally services (the host of all local
//     services, 127.0.0.1 by default) and the per-service overrides
//     fesl, gpcm, master, stella (host[:port]).
// The environment wins over the file. The retail client reads its CD key
// from the registry (HKLM\SOFTWARE\Electronic Arts\EA Games\Battlefield
// 2142\ergc, the default value; 0x00576d20 reads it through 0x0065d7e0,
// 0x00576e30 undoes the "x9392"-prefixed scrambling) and logs in with
// what the menu's login page is given.
#pragma once

#include <cstdint>
#include <string>

namespace dice::hfe::net {

struct ServiceEndpoint {
	std::string host;
	uint16_t port = 0;
	std::string text() const { return host + ":" + std::to_string(port); }
};

struct OnlineEndpoints {
	ServiceEndpoint fesl;   // EA's account service
	bool feslTls = true;    // OpenSpy's FESL is TLS; the local emulator plain TCP
	ServiceEndpoint gpcm;   // GameSpy Presence (gpcm.gamespy.com:29900, 0x00729420's connect)
	ServiceEndpoint master; // the server list (stella.ms<N>.<domain>:28910)
	ServiceEndpoint stella; // the stats web service (stella.prod.gamespy.com, HTTP; a server's ValidatePlayer)
	ServiceEndpoint cdkey;  // GameSpy's CD-key service (UDP 29910; servers only)
};

// OpenSpy's hosts, what the OpenSpy patch points the game at.
OnlineEndpoints liveEndpoints();
// Every service at `host` (127.0.0.1) on its usual port, FESL plain.
OnlineEndpoints localEndpoints(const std::string& host = "127.0.0.1");

struct OnlineCredentials {
	std::string account, password, soldier, cdKey;
	bool complete() const { return !account.empty() && !password.empty(); }
};

struct OnlineConfig {
	bool online = false; // --online: the live services; off, only this machine
	OnlineEndpoints endpoints = localEndpoints();
	OnlineCredentials credentials;
	std::string file; // where the credentials came from ("" none)
};

// Reads the credentials (environment and file, see above) and sets the
// endpoints for `online`.
OnlineConfig loadOnlineConfig(bool online);
// The same from a file's text (tests).
void applyOnlineConfigText(OnlineConfig& config, const std::string& text);

// The process's configuration (set once at start-up; offline until then).
const OnlineConfig& onlineConfig();
void setOnlineConfig(const OnlineConfig& config);

// Whether a host may be contacted: loopback and private (LAN) addresses
// always, anything else only online. Host names are allowed only online
// unless they are "localhost".
bool hostAllowed(const std::string& host);
// The refusal's text for a host that isn't allowed.
std::string hostRefusal(const std::string& host);

} // namespace dice::hfe::net
