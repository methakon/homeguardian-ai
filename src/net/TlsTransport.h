#pragma once

// TLS transport for the device ingestion boundary (milestone D-03).
//
// WHY NOT HAND-ROLLED: the brief requires an established TLS implementation with
// genuine peer authentication, and forbids custom cryptography. This wraps
// OpenSSL (TLS 1.2 minimum, reviewed defaults) for both directions:
//   * the SERVER side used by the hub's ingestion listener, and
//   * a CLIENT side used by the mock Android transport client in tests.
//
// AUTHENTICATION MODEL: mutual TLS against a private CA is the accepted approach
// for a local installation (the brief says so explicitly). A device certificate
// whose CN is the device id is issued per device; the hub then requires BOTH that
// the certificate chains to the CA AND that the fingerprint is the one bound to
// that device in the registry. The pair of bindings is what stops one phone from
// acting as another - neither alone is sufficient.
//
// WHAT IS NOT TRUSTED: the payload. The device id used for every authorization
// decision comes from the peer certificate, never from the message (see
// DeviceProtocol.h and ObservationIngest).
//
// BOUNDS: a hard frame cap, `SSL_read` on a socket with an explicit receive
// timeout, bounded buffers, and a fixed worker pool so a peer cannot make the hub
// allocate or spawn without limit.
//
// BUILD: this translation unit is compiled only when OpenSSL is available
// (CMake `HOMEGUARDIAN_WITH_TLS`). On a host without the OpenSSL development
// headers the protocol, registry, ingest and detector layers still build and test
// - only the socket layer is omitted, and CMake says so loudly.

#include "net/DeviceProtocol.h"

#include <cstdint>
#include <memory>
#include <string>

// Forward declarations so this header does not leak OpenSSL headers everywhere.
struct ssl_ctx_st;
struct ssl_st;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace homeguardian::net {

// One-time library initialisation. Safe to call repeatedly.
bool tls_library_init(std::string* error);

struct PeerIdentity {
    bool authenticated = false;
    std::string device_id;       // certificate CN
    std::string credential_id;   // SHA-256 fingerprint of the peer certificate (hex)
    bool verified_against_ca = false;
    std::string error;           // bounded reason when not authenticated
};

struct TlsServerConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 8443;
    std::string certificate_path;   // server certificate (PEM)
    std::string private_key_path;   // server key (PEM, 0600)
    std::string ca_path;            // private CA bundle (PEM)
    int handshake_timeout_ms = 5000;
    int idle_timeout_ms = 30000;      // no frame for this long -> close
    int write_timeout_ms = 5000;
    size_t max_frame_bytes = kMaxFrameBytes;
    size_t max_connections = 8;       // connection quota
    size_t max_workers = 2;           // bounded worker threads
    bool require_client_certificate = true;
};

// A single accepted TLS connection. Reads and writes length-prefixed messages.
class TlsConnection {
public:
    TlsConnection(SSL* ssl, int fd) : ssl_(ssl), fd_(fd) {}
    ~TlsConnection();
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    const PeerIdentity& peer() const { return peer_; }
    void set_peer(PeerIdentity peer) { peer_ = std::move(peer); }

    enum class ReadOutcome { message, idle_timeout, closed, error, oversized, bad_frame };

    // Read exactly one framed message. Bounded by `idle_timeout_ms`, and never
    // allocates more than max_frame_bytes.
    ReadOutcome read_message(DeviceMessage* out, RejectReason* reason, std::string* detail);

    // Write one framed message. False when the peer went away or the frame was
    // too large.
    bool write_message(const DeviceMessage& message);

    int fd() const { return fd_; }
    void close();

private:
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    PeerIdentity peer_{};
};

// The hub-side listener: binds, accepts, and hands each connection to a worker.
class TlsListener {
public:
    TlsListener() = default;
    ~TlsListener();
    TlsListener(const TlsListener&) = delete;
    TlsListener& operator=(const TlsListener&) = delete;

    // Loads the certificate, key and CA, then binds and listens. Returns false
    // with a bounded reason on any failure (missing files, key mismatch, a CA
    // that does not verify the server certificate, a bind failure).
    bool setup(const TlsServerConfig& config, std::string* error);

    // Accept one connection and complete the handshake within the timeout. On
    // timeout or a failed handshake returns nullptr; `error` explains which, and
    // `peer` is populated for the caller on success (its peer() carries the
    // verified identity).
    std::unique_ptr<TlsConnection> accept_one(std::string* error);

    int fd() const { return listen_fd_; }
    uint16_t port() const { return port_; }
    const TlsServerConfig& config() const { return config_; }
    void close_listener();

private:
    SSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    TlsServerConfig config_{};
};

// The device-side client, used by the mock transport client and (in principle) by
// an Android implementation. Requires the server certificate to verify against the
// same private CA - verification is never disabled.
class TlsClient {
public:
    TlsClient() = default;
    ~TlsClient();
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    bool connect(const std::string& host, uint16_t port, const std::string& ca_path,
                 const std::string& client_cert_path, const std::string& client_key_path,
                 int timeout_ms, std::string* error);

    const PeerIdentity& peer() const { return peer_; }
    bool send(const DeviceMessage& message, std::string* error);
    bool receive(DeviceMessage* out, int timeout_ms, std::string* error);
    void close();

private:
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    PeerIdentity peer_{};
};

}  // namespace homeguardian::net
