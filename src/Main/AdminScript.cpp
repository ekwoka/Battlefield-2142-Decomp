#include "AdminScript.h"

#include "ServerAdmin.h"

#include "Python/ScriptHost.h"

#include <cstdio>

namespace dice::hfe::host {

namespace {

// Runs once in the interpreter (it outlives the per-map hosts); see
// AdminScript.h.
constexpr const char* kSetup = R"PY(
def _bf2142_admin_setup():
	import sys, types, errno, hashlib, os, importlib
	import socket as _real
	import py2compat
	import host

	# socket.error with Python 2's indexing (detail[0] is the errno). A
	# would-block is EWOULDBLOCK whatever the platform calls it (Winsock's
	# 10035 is not errno.EWOULDBLOCK).
	class error(OSError):
		def __init__(self, *args):
			if len(args) == 1 and isinstance(args[0], BaseException):
				e = args[0]
				code = getattr(e, 'errno', None)
				if isinstance(e, (BlockingIOError, _real.timeout)):
					code = errno.EWOULDBLOCK
				args = (code if code is not None else 0, getattr(e, 'strerror', None) or str(e))
			OSError.__init__(self, *args)

		def __getitem__(self, index):
			return self.args[index]

	state = types.SimpleNamespace(loopback=True, module=None)

	def call(function, *args):
		try:
			return function(*args)
		except error:
			raise
		except OSError as e:
			raise error(e)

	class socket(object):
		def __init__(self, family=_real.AF_INET, type=_real.SOCK_STREAM, proto=0, _sock=None):
			self._sock = _sock if _sock is not None else _real.socket(family, type, proto)

		def bind(self, address):
			if state.loopback and isinstance(address, tuple) and address[0] in ('', '0.0.0.0'):
				address = ('127.0.0.1',) + tuple(address[1:])
			return call(self._sock.bind, address)

		def accept(self):
			conn, peer = call(self._sock.accept)
			return socket(_sock=conn), peer

		def send(self, data, *flags):
			if isinstance(data, str):
				data = data.encode('latin-1')
			return call(self._sock.send, data, *flags)

		def sendall(self, data, *flags):
			if isinstance(data, str):
				data = data.encode('latin-1')
			return call(self._sock.sendall, data, *flags)

		def sendto(self, data, *rest):
			if isinstance(data, str):
				data = data.encode('latin-1')
			return call(self._sock.sendto, data, *rest)

		def recv(self, size, *flags):
			return call(self._sock.recv, size, *flags).decode('latin-1')

		def recvfrom(self, size, *flags):
			data, peer = call(self._sock.recvfrom, size, *flags)
			return data.decode('latin-1'), peer

		def setblocking(self, flag):
			self._sock.setblocking(bool(flag))

		# A peer that has gone: Winsock's shutdown doesn't mind, BSD
		# sockets say ENOTCONN (default.py's handler of that would fail).
		def shutdown(self, how):
			try:
				self._sock.shutdown(how)
			except OSError as e:
				if e.errno != errno.ENOTCONN:
					raise error(e)

		def __getattr__(self, name):
			attr = getattr(self._sock, name)
			if callable(attr):
				return lambda *args: call(attr, *args)
			return attr

	module = types.ModuleType('py2socket')
	for name in dir(_real):
		if not name.startswith('_'):
			setattr(module, name, getattr(_real, name))
	module.socket = module.SocketType = socket
	module.error = error
	sys.modules['py2socket'] = module
	py2compat.REDIRECTS['socket'] = 'py2socket'

	# md5 objects that take str (default.py digests seed + password).
	class md5(object):
		digest_size = 16

		def __init__(self, data=''):
			self._hash = hashlib.md5()
			if data:
				self.update(data)

		def update(self, data):
			self._hash.update(data.encode('latin-1') if isinstance(data, str) else data)

		def digest(self):
			return self._hash.digest().decode('latin-1')

		def hexdigest(self):
			return self._hash.hexdigest()

		def copy(self):
			other = md5()
			other._hash = self._hash.copy()
			return other

	md5module = types.ModuleType('md5')
	md5module.new = md5module.md5 = md5
	md5module.digest_size = 16
	sys.modules['md5'] = md5module

	# The handlers and timers the admin scripts register, for the next
	# map's host.
	real = dict((name, getattr(host, name)) for name in
		('registerHandler', 'unregisterHandler', 'timer_created', 'timer_destroy'))
	handlers = []
	timers = []

	def fromAdmin():
		frame = sys._getframe(2)
		while frame is not None:
			name = frame.f_globals.get('__name__', '') or ''
			if name == 'admin' or name.startswith('admin.'):
				return True
			frame = frame.f_back
		return False

	def registerHandler(*args):
		if fromAdmin():
			handlers.append(args)
		return real['registerHandler'](*args)

	def unregisterHandler(*args):
		if fromAdmin():
			handlers[:] = [h for h in handlers if not (h[0] == args[0] and len(args) > 1 and h[1] == args[1])]
		return real['unregisterHandler'](*args)

	def timer_created(timer):
		if fromAdmin():
			timers.append(timer)
		return real['timer_created'](timer)

	def timer_destroy(timer):
		if timer in timers:
			timers.remove(timer)
		return real['timer_destroy'](timer)

	host.registerHandler = registerHandler
	host.unregisterHandler = unregisterHandler
	host.timer_created = timer_created
	host.timer_destroy = timer_destroy

	def start(directory, name, loopback):
		state.loopback = bool(loopback)
		full = os.path.abspath(directory)
		if full not in py2compat._game_roots:
			py2compat._game_roots.append(full)
		sys.path_importer_cache.clear()
		package = types.ModuleType('admin')
		package.__path__ = [full]
		package.__package__ = 'admin'
		package.__file__ = os.path.join(full, '__init__.py')
		sys.modules['admin'] = package
		state.module = importlib.import_module('admin.' + name)
		if hasattr(state.module, 'init'):
			state.module.init()

	def replay():
		for args in list(handlers):
			real['registerHandler'](*args)
		now = host.timer_getWallTime()
		for timer in list(timers):
			interval = getattr(timer, 'interval', 0) or 0
			try:
				timer.time = now + (interval if interval > 0 else 1)
			except Exception:
				pass
			real['timer_created'](timer)

	def update():
		if state.module is not None and hasattr(state.module, 'update'):
			state.module.update()

	def shutdown():
		if state.module is not None and hasattr(state.module, 'shutdown'):
			state.module.shutdown()
		state.module = None

	return types.SimpleNamespace(start=start, replay=replay, update=update, shutdown=shutdown, state=state)

if '_bf2142_admin' not in globals():
	_bf2142_admin = _bf2142_admin_setup()
)PY";

std::string quoted(const std::string& text)
{
	std::string out = "'";
	for (const char c : text) {
		if (c == '\\' || c == '\'') {
			out += '\\';
		}
		out += c;
	}
	return out + "'";
}

} // namespace

bool AdminScript::start(script::ScriptHost& scripts, const std::string& gameDirectory, const std::string& name,
	bool loopbackOnly)
{
	m_active = false;
	if (name.empty() || !scripts.ok()) {
		return false;
	}
	if (m_started) {
		m_active = scripts.run("_bf2142_admin.replay()");
		return m_active;
	}
	if (!scripts.run(kSetup)) {
		std::fprintf(stderr, "admin: the script support did not load\n");
		return false;
	}
	// The install's folder is "Admin" (the scripts open it as admin).
	const std::string directory = findFileNoCase(gameDirectory + "/admin");
	m_started = scripts.run("_bf2142_admin.start(" + quoted(directory) + ", " + quoted(name) + ", "
		+ (loopbackOnly ? "True" : "False") + ")");
	m_active = m_started;
	if (!m_started) {
		std::fprintf(stderr, "admin: admin.%s did not start\n", name.c_str());
	}
	return m_started;
}

void AdminScript::update(script::ScriptHost& scripts)
{
	if (m_active) {
		scripts.run("_bf2142_admin.update()");
	}
}

void AdminScript::shutdown(script::ScriptHost& scripts)
{
	if (m_started) {
		scripts.run("_bf2142_admin.shutdown()");
		m_started = m_active = false;
	}
}

} // namespace dice::hfe::host
