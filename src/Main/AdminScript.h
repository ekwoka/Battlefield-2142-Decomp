// The server's admin script (sv.adminScript, "default": the install's
// admin/default.py, the TCP remote console, which imports
// admin/standard_admin: team auto-balance, team-kill punishing and the
// player connect checks). The engine imports admin.<sv.adminScript>,
// calls its init() once and its update() every frame, and shutdown() at
// the end.
//
// The scripts are Python 2 on the rewrite's Python 3 (Python/py2compat.py
// translates them); this adds what they need beyond that: a socket module
// with Python 2's str API and errors that index like tuples (detail[0]),
// md5 objects that take str, and the admin package from the install's
// admin (or Admin) folder. On a loopback-only server a bind to 0.0.0.0 is
// made to 127.0.0.1.
//
// One ScriptHost is made per map, but the interpreter and the admin
// module live on (as on the original server, the remote console's
// connections survive a map change): the handlers and timers the admin
// scripts registered are recorded and registered again on each new map's
// host.
#pragma once

#include <string>

namespace dice::hfe::script {
class ScriptHost;
}

namespace dice::hfe::host {

class AdminScript {
public:
	// Imports admin.<name> (first map) or registers its handlers again
	// (later maps). False when it could not be loaded.
	bool start(script::ScriptHost& scripts, const std::string& gameDirectory, const std::string& name,
		bool loopbackOnly);
	// update() of the admin module (every frame).
	void update(script::ScriptHost& scripts);
	// shutdown() of the admin module (the server stops).
	void shutdown(script::ScriptHost& scripts);
	bool started() const { return m_started; }

private:
	bool m_started = false;
	bool m_active = false; // started on the current host
};

} // namespace dice::hfe::host
