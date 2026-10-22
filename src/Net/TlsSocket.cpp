#include "TlsSocket.h"

#include "Socket.h"

#ifdef BF2142_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

namespace dice::hfe::net {

#ifdef BF2142_HAVE_TLS

struct TlsSocket::State {
	TcpSocket tcp;
	SSL_CTX* context = nullptr;
	SSL* ssl = nullptr;
	~State()
	{
		if (ssl) {
			SSL_shutdown(ssl);
			SSL_free(ssl);
		}
		if (context) {
			SSL_CTX_free(context);
		}
	}
};

bool TlsSocket::available() { return true; }

bool TlsSocket::connect(const std::string& host, uint16_t port, int timeoutSeconds, std::string* error)
{
	const auto fail = [&](const std::string& why) {
		if (error) {
			*error = why;
		}
		m_state.reset();
		return false;
	};
	m_state = std::make_unique<State>();
	if (!m_state->tcp.connect(host, port, timeoutSeconds)) {
		return fail("cannot connect to " + host + ":" + std::to_string(port));
	}
	m_state->context = SSL_CTX_new(TLS_client_method());
	if (!m_state->context) {
		return fail("cannot create a TLS context");
	}
	SSL_CTX_set_default_verify_paths(m_state->context);
	SSL_CTX_set_verify(m_state->context, SSL_VERIFY_PEER, nullptr);
	m_state->ssl = SSL_new(m_state->context);
	SSL_set_tlsext_host_name(m_state->ssl, host.c_str());
	SSL_set1_host(m_state->ssl, host.c_str());
	SSL_set_fd(m_state->ssl, static_cast<int>(m_state->tcp.native()));
	if (SSL_connect(m_state->ssl) != 1) {
		char text[256];
		ERR_error_string_n(ERR_get_error(), text, sizeof(text));
		return fail(std::string("TLS handshake failed: ") + text);
	}
	return true;
}

bool TlsSocket::send(const std::vector<uint8_t>& data)
{
	if (!m_state) {
		return false;
	}
	return data.empty() || SSL_write(m_state->ssl, data.data(), static_cast<int>(data.size())) == static_cast<int>(data.size());
}

int TlsSocket::receive(std::vector<uint8_t>& out, int timeoutMilliseconds)
{
	if (!m_state) {
		return -1;
	}
	if (SSL_pending(m_state->ssl) == 0) {
		const auto s = m_state->tcp.native();
		fd_set set;
		FD_ZERO(&set);
#ifdef _WIN32
		FD_SET(static_cast<SOCKET>(s), &set);
#else
		FD_SET(static_cast<int>(s), &set);
#endif
		timeval timeout {timeoutMilliseconds / 1000, (timeoutMilliseconds % 1000) * 1000};
		if (select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &timeout) <= 0) {
			return 0;
		}
	}
	uint8_t buffer[4096];
	const int read = SSL_read(m_state->ssl, buffer, sizeof(buffer));
	if (read <= 0) {
		const int why = SSL_get_error(m_state->ssl, read);
		return why == SSL_ERROR_WANT_READ || why == SSL_ERROR_WANT_WRITE ? 0 : -1;
	}
	out.insert(out.end(), buffer, buffer + read);
	return read;
}

#else

struct TlsSocket::State {};

bool TlsSocket::available() { return false; }

bool TlsSocket::connect(const std::string&, uint16_t, int, std::string* error)
{
	if (error) {
		*error = "built without TLS (OpenSSL was not found)";
	}
	return false;
}

bool TlsSocket::send(const std::vector<uint8_t>&) { return false; }
int TlsSocket::receive(std::vector<uint8_t>&, int) { return -1; }

#endif

TlsSocket::TlsSocket() = default;
TlsSocket::~TlsSocket() = default;
void TlsSocket::close() { m_state.reset(); }

} // namespace dice::hfe::net
