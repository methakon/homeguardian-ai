#include "net/TlsTransport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <cstring>

namespace homeguardian::net {

namespace {

std::string bounded(std::string text, size_t max_chars = 128) {
    if (text.size() > max_chars) text.resize(max_chars);
    for (char& c : text) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return text;
}

// The last OpenSSL error, bounded and stripped of anything resembling a secret.
std::string last_ssl_error(const std::string& context) {
    const unsigned long code = ERR_get_error();
    if (code == 0) return bounded(context);
    char buffer[256] = {0};
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return bounded(context + ": " + std::string(buffer));
}

std::string fingerprint_sha256(X509* certificate) {
    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned int length = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &length) != 1) return std::string();
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0xF]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

std::string common_name(X509* certificate) {
    if (certificate == nullptr) return std::string();
    char buffer[256] = {0};
    const int written = X509_NAME_get_text_by_NID(X509_get_subject_name(certificate),
                                                 NID_commonName, buffer, sizeof(buffer));
    if (written <= 0) return std::string();
    return bounded(std::string(buffer, static_cast<size_t>(written)), 96);
}

void set_socket_timeouts(int fd, int timeout_ms) {
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Full-write helper: SSL_write may accept only part of a buffer.
bool ssl_write_all(SSL* ssl, const uint8_t* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const int n = SSL_write(ssl, data + written, static_cast<int>(size - written));
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

// Why a read stopped. `SSL_get_error` must be consulted immediately after the
// failing SSL_read, so the classification happens HERE rather than at the call
// site - otherwise an idle timeout and a protocol error become indistinguishable
// and the server cannot implement a timeout policy honestly.
enum class IoOutcome { progress, eof, want_io, error };

IoOutcome ssl_read_some(SSL* ssl, uint8_t* data, size_t size, int* out_bytes) {
    const int n = SSL_read(ssl, data, static_cast<int>(size));
    if (n > 0) {
        if (out_bytes != nullptr) *out_bytes = n;
        return IoOutcome::progress;
    }
    switch (SSL_get_error(ssl, n)) {
        case SSL_ERROR_ZERO_RETURN:
            return IoOutcome::eof;
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return IoOutcome::want_io;  // socket timeout: no data within the window
        default:
            return IoOutcome::error;
    }
}

// Read exactly `size` bytes. `saw_progress` distinguishes "nothing arrived at
// all" (idle) from "the frame was cut short" (protocol error).
bool ssl_read_exact_ex(SSL* ssl, uint8_t* data, size_t size, bool* saw_progress) {
    size_t got = 0;
    while (got < size) {
        int n = 0;
        const IoOutcome outcome = ssl_read_some(ssl, data + got, size - got, &n);
        if (outcome != IoOutcome::progress) return false;
        got += static_cast<size_t>(n);
        if (saw_progress != nullptr) *saw_progress = true;
    }
    return true;
}

bool connect_tcp(const std::string& host, uint16_t port, int timeout_ms, int* out_fd,
                 std::string* error) {
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        if (error != nullptr) *error = "host must be a dotted IPv4 address";
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error != nullptr) *error = "socket() failed";
        return false;
    }
    set_socket_timeouts(fd, timeout_ms);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) *error = std::string("connect() failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    *out_fd = fd;
    return true;
}

// Verify the peer certificate chain and produce the identity the hub will use.
bool evaluate_peer(SSL* ssl, PeerIdentity* identity, std::string* error) {
    X509* certificate = SSL_get1_peer_certificate(ssl);
    if (certificate == nullptr) {
        if (error != nullptr) *error = "peer presented no certificate";
        return false;
    }
    const long verify_result = SSL_get_verify_result(ssl);
    if (verify_result != X509_V_OK) {
        if (error != nullptr) {
            *error = bounded(std::string("peer certificate failed verification: ") +
                             X509_verify_cert_error_string(verify_result));
        }
        X509_free(certificate);
        return false;
    }
    identity->verified_against_ca = true;
    identity->credential_id = fingerprint_sha256(certificate);
    identity->device_id = common_name(certificate);
    if (identity->device_id.empty() || identity->credential_id.empty()) {
        if (error != nullptr) *error = "peer certificate has no usable CN or fingerprint";
        X509_free(certificate);
        return false;
    }
    identity->authenticated = true;
    X509_free(certificate);
    return true;
}

}  // namespace

bool tls_library_init(std::string* error) {
    // OPENSSL_init_ssl is idempotent; calling it once per process is enough.
    static bool initialised = false;
    if (!initialised) {
        if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                             nullptr) != 1) {
            if (error != nullptr) *error = last_ssl_error("OPENSSL_init_ssl failed");
            return false;
        }
        initialised = true;
    }
    if (error != nullptr) error->clear();
    return true;
}

// ---------------------------------------------------------------------------
// TlsConnection
// ---------------------------------------------------------------------------

TlsConnection::~TlsConnection() { close(); }

void TlsConnection::close() {
    if (ssl_ != nullptr) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);  // frees the BIO chain attached to the socket
        ssl_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

TlsConnection::ReadOutcome TlsConnection::read_message(DeviceMessage* out, RejectReason* reason,
                                                       std::string* detail) {
    if (ssl_ == nullptr) return ReadOutcome::error;

    uint8_t header[4] = {0};
    bool saw_progress = false;
    for (size_t got = 0; got < sizeof(header);) {
        int n = 0;
        const IoOutcome outcome = ssl_read_some(ssl_, header + got, sizeof(header) - got, &n);
        if (outcome == IoOutcome::eof) return ReadOutcome::closed;
        if (outcome == IoOutcome::want_io) {
            // Nothing at all within the window is an IDLE connection (a healthy
            // case the server closes quietly); a stall mid-header is an error.
            return saw_progress ? ReadOutcome::error : ReadOutcome::idle_timeout;
        }
        if (outcome == IoOutcome::error) return ReadOutcome::error;
        got += static_cast<size_t>(n);
        saw_progress = true;
    }

    size_t declared = 0;
    if (!frame_length_acceptable(header, sizeof(header), &declared)) {
        if (reason != nullptr) *reason = RejectReason::oversized;
        if (detail != nullptr) *detail = "declared frame length is zero or beyond the cap";
        return ReadOutcome::oversized;
    }

    std::string body(declared, '\0');
    bool body_progress = false;
    if (!ssl_read_exact_ex(ssl_, reinterpret_cast<uint8_t*>(&body[0]), declared, &body_progress)) {
        if (reason != nullptr) *reason = RejectReason::bad_frame;
        if (detail != nullptr) {
            *detail = body_progress ? "connection closed or stalled mid-frame"
                                    : "no body arrived within the idle window";
        }
        return ReadOutcome::bad_frame;
    }

    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        if (reason != nullptr) *reason = RejectReason::bad_json;
        if (detail != nullptr) *detail = "frame is not valid JSON";
        return ReadOutcome::bad_frame;
    }

    RejectReason decode_reason = RejectReason::none;
    std::string decode_detail;
    const auto message = decode(parsed, &decode_reason, &decode_detail);
    if (!message.has_value()) {
        if (reason != nullptr) *reason = decode_reason;
        if (detail != nullptr) *detail = decode_detail;
        return ReadOutcome::bad_frame;
    }
    if (reason != nullptr) *reason = RejectReason::none;
    if (out != nullptr) *out = message.value();
    return ReadOutcome::message;
}

bool TlsConnection::write_message(const DeviceMessage& message) {
    if (ssl_ == nullptr) return false;
    const std::string body = encode(message).dump();
    if (body.size() > kMaxFrameBytes) return false;
    const uint32_t length = static_cast<uint32_t>(body.size());
    const uint8_t header[4] = {static_cast<uint8_t>((length >> 24) & 0xFF),
                               static_cast<uint8_t>((length >> 16) & 0xFF),
                               static_cast<uint8_t>((length >> 8) & 0xFF),
                               static_cast<uint8_t>(length & 0xFF)};
    if (!ssl_write_all(ssl_, header, sizeof(header))) return false;
    return ssl_write_all(ssl_, reinterpret_cast<const uint8_t*>(body.data()), body.size());
}

// ---------------------------------------------------------------------------
// TlsListener
// ---------------------------------------------------------------------------

TlsListener::~TlsListener() { close_listener(); }

void TlsListener::close_listener() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

bool TlsListener::setup(const TlsServerConfig& config, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = bounded(why);
        return false;
    };
    config_ = config;
    if (!tls_library_init(error)) return false;
    if (config.certificate_path.empty() || config.private_key_path.empty() ||
        config.ca_path.empty()) {
        return fail("certificate, private key and CA paths are all required");
    }

    // TLS 1.2 minimum and OpenSSL's reviewed default cipher suite: no custom
    // crypto, no downgrade path.
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == nullptr) return fail(last_ssl_error("SSL_CTX_new failed"));
    if (SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION) != 1) {
        return fail("could not require TLS 1.2");
    }
    SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);

    if (SSL_CTX_use_certificate_chain_file(ctx_, config.certificate_path.c_str()) != 1) {
        return fail(last_ssl_error("could not load the server certificate"));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, config.private_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        return fail(last_ssl_error("could not load the server private key"));
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        return fail("the server private key does not match the certificate");
    }
    if (SSL_CTX_load_verify_locations(ctx_, config.ca_path.c_str(), nullptr) != 1) {
        return fail(last_ssl_error("could not load the private CA bundle"));
    }
    SSL_CTX_set_verify(ctx_,
                       config.require_client_certificate
                           ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                           : SSL_VERIFY_NONE,
                       nullptr);
    SSL_CTX_set_verify_depth(ctx_, 2);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return fail("socket() failed");
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (config.bind_address == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, config.bind_address.c_str(), &address.sin_addr) != 1) {
        return fail("bind_address must be a dotted IPv4 address");
    }
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        return fail(std::string("bind() failed: ") + std::strerror(errno));
    }
    if (::listen(listen_fd_, static_cast<int>(config.max_connections)) != 0) {
        return fail(std::string("listen() failed: ") + std::strerror(errno));
    }

    // Report the port actually bound (port 0 means "pick one", which the tests use).
    struct sockaddr_in bound {};
    socklen_t length = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&bound), &length) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = config.port;
    }
    if (error != nullptr) error->clear();
    return true;
}

std::unique_ptr<TlsConnection> TlsListener::accept_one(std::string* error) {
    if (listen_fd_ < 0 || ctx_ == nullptr) {
        if (error != nullptr) *error = "listener is not set up";
        return nullptr;
    }

    // A short accept timeout keeps the loop responsive to shutdown requests.
    struct timeval tv {};
    tv.tv_sec = 0;
    tv.tv_usec = 500 * 1000;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in peer_address {};
    socklen_t peer_length = sizeof(peer_address);
    const int fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer_address),
                            &peer_length);
    if (fd < 0) {
        if (error != nullptr) *error = (errno == EAGAIN || errno == EWOULDBLOCK) ? "accept timeout"
                                                                                : "accept failed";
        return nullptr;
    }
    set_socket_timeouts(fd, config_.handshake_timeout_ms);

    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr) {
        ::close(fd);
        if (error != nullptr) *error = last_ssl_error("SSL_new failed");
        return nullptr;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) != 1) {
        if (error != nullptr) *error = last_ssl_error("TLS handshake failed");
        SSL_free(ssl);
        ::close(fd);
        return nullptr;
    }

    PeerIdentity identity;
    std::string verify_error;
    if (config_.require_client_certificate && !evaluate_peer(ssl, &identity, &verify_error)) {
        if (error != nullptr) *error = verify_error;
        SSL_free(ssl);
        ::close(fd);
        return nullptr;
    }
    if (!config_.require_client_certificate) {
        identity.authenticated = false;
    }

    // From here the connection works on its own timeouts.
    set_socket_timeouts(fd, config_.idle_timeout_ms);
    auto connection = std::make_unique<TlsConnection>(ssl, fd);
    connection->set_peer(identity);
    if (error != nullptr) error->clear();
    return connection;
}

// ---------------------------------------------------------------------------
// TlsClient
// ---------------------------------------------------------------------------

TlsClient::~TlsClient() { close(); }

void TlsClient::close() {
    if (ssl_ != nullptr) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

bool TlsClient::connect(const std::string& host, uint16_t port, const std::string& ca_path,
                        const std::string& client_cert_path, const std::string& client_key_path,
                        int timeout_ms, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = bounded(why);
        return false;
    };
    if (!tls_library_init(error)) return false;

    ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) return fail(last_ssl_error("SSL_CTX_new failed"));
    if (SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION) != 1) {
        return fail("could not require TLS 1.2");
    }
    // Verification is required and is never disabled for convenience: the peer
    // certificate must chain to the private CA.
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_load_verify_locations(ctx_, ca_path.c_str(), nullptr) != 1) {
        return fail(last_ssl_error("could not load the private CA bundle"));
    }
    if (SSL_CTX_use_certificate_chain_file(ctx_, client_cert_path.c_str()) != 1) {
        return fail(last_ssl_error("could not load the client certificate"));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, client_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        return fail(last_ssl_error("could not load the client private key"));
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        return fail("the client private key does not match the certificate");
    }

    int fd = -1;
    if (!connect_tcp(host, port, timeout_ms, &fd, error)) return false;
    fd_ = fd;

    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr) return fail(last_ssl_error("SSL_new failed"));
    SSL_set_fd(ssl_, fd_);
    if (SSL_connect(ssl_) != 1) return fail(last_ssl_error("TLS handshake failed"));

    PeerIdentity identity;
    std::string verify_error;
    if (!evaluate_peer(ssl_, &identity, &verify_error)) return fail(verify_error);
    peer_ = identity;

    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (error != nullptr) error->clear();
    return true;
}

bool TlsClient::send(const DeviceMessage& message, std::string* error) {
    if (ssl_ == nullptr) {
        if (error != nullptr) *error = "not connected";
        return false;
    }
    const std::string body = encode(message).dump();
    if (body.size() > kMaxFrameBytes) {
        if (error != nullptr) *error = "message exceeds the frame cap";
        return false;
    }
    const uint32_t length = static_cast<uint32_t>(body.size());
    const uint8_t header[4] = {static_cast<uint8_t>((length >> 24) & 0xFF),
                               static_cast<uint8_t>((length >> 16) & 0xFF),
                               static_cast<uint8_t>((length >> 8) & 0xFF),
                               static_cast<uint8_t>(length & 0xFF)};
    if (!ssl_write_all(ssl_, header, sizeof(header))) {
        if (error != nullptr) *error = last_ssl_error("write failed");
        return false;
    }
    if (!ssl_write_all(ssl_, reinterpret_cast<const uint8_t*>(body.data()), body.size())) {
        if (error != nullptr) *error = last_ssl_error("write failed");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool TlsClient::receive(DeviceMessage* out, int timeout_ms, std::string* error) {
    if (ssl_ == nullptr) {
        if (error != nullptr) *error = "not connected";
        return false;
    }
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t header[4] = {0};
    if (!ssl_read_exact_ex(ssl_, header, sizeof(header), nullptr)) {
        if (error != nullptr) *error = "no complete frame within the timeout";
        return false;
    }
    size_t declared = 0;
    if (!frame_length_acceptable(header, sizeof(header), &declared)) {
        if (error != nullptr) *error = "declared frame length is unusable";
        return false;
    }
    std::string body(declared, '\0');
    if (!ssl_read_exact_ex(ssl_, reinterpret_cast<uint8_t*>(&body[0]), declared, nullptr)) {
        if (error != nullptr) *error = "connection closed mid-frame";
        return false;
    }
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        if (error != nullptr) *error = "frame is not valid JSON";
        return false;
    }
    RejectReason reason = RejectReason::none;
    std::string detail;
    const auto message = decode(parsed, &reason, &detail);
    if (!message.has_value()) {
        if (error != nullptr) *error = "frame did not decode: " + detail;
        return false;
    }
    if (out != nullptr) *out = message.value();
    if (error != nullptr) error->clear();
    return true;
}

}  // namespace homeguardian::net
