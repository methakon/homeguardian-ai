#include "net/IngressHealthProbe.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>

namespace homeguardian::net {

namespace {

// Records what OpenSSL's own verification concluded, so a handshake that ends at the server's
// client-certificate demand can be told apart from one that failed to verify the server.
struct VerifyState {
    bool leaf_verified = false;   // depth-0 (the server's own certificate) passed, name check included
    std::string first_error;      // the first verification error, if any
};

// What the server actually sent, straight off the handshake message stream. This is the reliable
// answer to "is mutual TLS demanded?" - see the header note about TLS 1.3 timing.
struct MsgTrace {
    bool certificate_request_seen = false;
};

void message_callback(int write_p, int /*version*/, int content_type, const void* buf, size_t len,
                      SSL* /*ssl*/, void* arg) {
    if (write_p != 0 || arg == nullptr) return;              // inbound records only
    if (content_type != SSL3_RT_HANDSHAKE || buf == nullptr || len == 0) return;
    const unsigned char* first = static_cast<const unsigned char*>(buf);
    if (first[0] == SSL3_MT_CERTIFICATE_REQUEST) {
        static_cast<MsgTrace*>(arg)->certificate_request_seen = true;
    }
}

int verify_callback(int preverify_ok, X509_STORE_CTX* store_ctx) {
    SSL* ssl = static_cast<SSL*>(
        X509_STORE_CTX_get_ex_data(store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
    VerifyState* state = ssl ? static_cast<VerifyState*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)))
                             : nullptr;
    if (state != nullptr) {
        if (preverify_ok) {
            // depth 0 is the server's own certificate; the SAN/name check is part of this result,
            // so a wrong-name certificate never reaches here as ok.
            if (X509_STORE_CTX_get_error_depth(store_ctx) == 0) state->leaf_verified = true;
        } else if (state->first_error.empty()) {
            state->first_error = X509_verify_cert_error_string(X509_STORE_CTX_get_error(store_ctx));
        }
    }
    return preverify_ok;  // never override: a certificate that fails must fail the handshake
}

// Bounded, allocation-light description of a name. Diagnostic only - no key material.
std::string name_to_string(X509_NAME* name) {
    if (name == nullptr) return "(none)";
    char buffer[256];
    const int written = X509_NAME_get_text_by_NID(name, NID_commonName, buffer, sizeof(buffer));
    if (written <= 0) return "(no CN)";
    return std::string("CN=") + buffer;
}

// The certificate's SAN entries, so an operator can SEE the name that did not match instead of
// guessing it. Bounded: the first few entries, each truncated.
std::string san_to_string(X509* cert) {
    if (cert == nullptr) return "(none)";
    GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (names == nullptr) return "(no SAN)";
    std::string out;
    const int count = sk_GENERAL_NAME_num(names);
    int emitted = 0;
    for (int i = 0; i < count && emitted < 4; ++i) {
        const GENERAL_NAME* entry = sk_GENERAL_NAME_value(names, i);
        if (entry == nullptr) continue;
        char value[128] = {0};
        if (entry->type == GEN_DNS) {
            const ASN1_IA5STRING* dns = entry->d.dNSName;
            const int len = ASN1_STRING_length(dns) < 120 ? ASN1_STRING_length(dns) : 120;
            std::memcpy(value, ASN1_STRING_get0_data(dns), static_cast<size_t>(len));
            out += (emitted ? "," : "");
            out += std::string("DNS:") + value;
            ++emitted;
        } else if (entry->type == GEN_IPADD) {
            const ASN1_OCTET_STRING* ip = entry->d.iPAddress;
            if (ip != nullptr && ASN1_STRING_length(ip) == 4) {
                const unsigned char* raw = ASN1_STRING_get0_data(ip);
                char text[INET_ADDRSTRLEN] = {0};
                if (inet_ntop(AF_INET, raw, text, sizeof(text)) != nullptr) {
                    out += (emitted ? "," : "");
                    out += std::string("IP:") + text;
                    ++emitted;
                }
            }
        }
    }
    GENERAL_NAMES_free(names);
    return out.empty() ? "(no printable SAN entry)" : out;
}

int connect_with_timeout(const std::string& host, int port, int timeout_ms, std::string* error) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* list = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &list);
    if (rc != 0 || list == nullptr) {
        *error = std::string("cannot resolve ") + host + ": " + gai_strerror(rc);
        return -1;
    }

    int fd = -1;
    std::string last_error = "no address attempted";
    for (struct addrinfo* candidate = list; candidate != nullptr; candidate = candidate->ai_next) {
        fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) {
            last_error = "socket() failed";
            continue;
        }
        // Non-blocking connect + poll so a filtered or wedged endpoint cannot hang the health check.
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const int cr = ::connect(fd, candidate->ai_addr, candidate->ai_addrlen);
        if (cr == 0) {
            ::fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            const int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                    ::fcntl(fd, F_SETFL, flags);
                    break;
                }
                last_error = std::string("connect failed: ") + std::strerror(so_error);
            } else {
                last_error = (pr == 0) ? "connect timed out" : "poll failed";
            }
        } else {
            last_error = std::string("connect failed: ") + std::strerror(errno);
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(list);
    if (fd < 0) *error = last_error;
    return fd;
}

} // namespace

std::string IngressProbeResult::summary() const {
    std::string out = ok ? "hg_ingress_probe: OK" : "hg_ingress_probe: FAIL";
    if (!endpoint.empty()) out += " endpoint=" + endpoint;
    if (!tls_version.empty()) out += " tls=" + tls_version;
    if (!server_subject.empty()) out += " server=" + server_subject;
    if (!server_san.empty()) out += " san=" + server_san;
    out += std::string(" server_cert_verified=") + (server_cert_verified ? "yes" : "no");
    out += std::string(" mutual_tls_demanded=") + (mutual_tls_demanded ? "yes" : "no");
    // Never let this line be read as device authentication.
    out += " device_auth=NOT_PERFORMED";
    if (!detail.empty()) out += " detail=" + detail;
    return out;
}

IngressProbeResult probe_ingress_endpoint(const IngressProbeOptions& options) {
    IngressProbeResult result;

    if (options.ca_file.empty()) {
        result.detail = "no CA file configured (chain verification cannot be skipped)";
        return result;
    }
    if (options.verify_name.empty() && options.verify_ip.empty()) {
        result.detail = "no expected name or address configured (identity check cannot be skipped)";
        return result;
    }

    result.endpoint = options.connect_host + ":" + std::to_string(options.port);

    std::string connect_error;
    const int fd = connect_with_timeout(options.connect_host, options.port, options.timeout_ms, &connect_error);
    if (fd < 0) {
        result.detail = connect_error;
        return result;
    }
    result.connected = true;

    // Bound the TLS exchange so a peer that accepts the connection and then stalls cannot hold the
    // health check open.
    struct timeval tv;
    tv.tv_sec = options.timeout_ms / 1000;
    tv.tv_usec = (options.timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    VerifyState verify_state;
    MsgTrace msg_trace;
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        ::close(fd);
        result.detail = "SSL_CTX_new failed";
        return result;
    }
    SSL_CTX_set_app_data(ctx, &verify_state);
    // (a) Watch the handshake messages themselves: a CertificateRequest means mutual TLS is demanded.
    SSL_CTX_set_msg_callback(ctx, message_callback);
    SSL_CTX_set_msg_callback_arg(ctx, &msg_trace);

    do {
        // TLS 1.2 floor, exactly as the device client and the ingress server enforce.
        if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
            result.detail = "cannot set the TLS 1.2 minimum";
            break;
        }
        // Server certificate: verify against the private CA. SSL_VERIFY_PEER only - there is no
        // SSL_VERIFY_NONE path anywhere in this probe.
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verify_callback);
        SSL_CTX_set_verify_depth(ctx, 2);
        if (SSL_CTX_load_verify_locations(ctx, options.ca_file.c_str(), nullptr) != 1) {
            result.detail = std::string("cannot load the CA certificate: ") + options.ca_file;
            break;
        }

        SSL* ssl = SSL_new(ctx);
        if (ssl == nullptr) {
            result.detail = "SSL_new failed";
            break;
        }
        SSL_set_fd(ssl, fd);

        // Pin the EXPECTED identity (SAN, never CN): the same mechanism as the device client, so a
        // certificate for another endpoint cannot satisfy this probe.
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        if (!options.verify_ip.empty()) {
            if (X509_VERIFY_PARAM_set1_ip_asc(param, options.verify_ip.c_str()) != 1) {
                result.detail = "cannot set the expected address for verification";
                SSL_free(ssl);
                    break;
            }
        } else {
            X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
            if (X509_VERIFY_PARAM_set1_host(param, options.verify_name.c_str(), 0) != 1) {
                result.detail = "cannot set the expected name for verification";
                SSL_free(ssl);
                    break;
            }
            SSL_set_tlsext_host_name(ssl, options.verify_name.c_str());
        }

        result.handshake_started = true;
        const int connected_ok = SSL_connect(ssl);
        const long verify_code = SSL_get_verify_result(ssl);

        if (X509* peer = SSL_get1_peer_certificate(ssl)) {
            result.server_subject = name_to_string(X509_get_subject_name(peer));
            result.server_san = san_to_string(peer);
            X509_free(peer);
        }
        if (const char* version = SSL_get_version(ssl)) result.tls_version = version;

        // The server's identity counts as verified only when BOTH OpenSSL's own name/chain check on
        // the leaf passed AND the overall verification result is X509_V_OK.
        result.server_cert_verified = verify_state.leaf_verified && verify_code == X509_V_OK;

        if (connected_ok != 1) {
            // The handshake aborted. With a client-certificate demand that is the EXPECTED end of a
            // certificate-less client handshake, so it is evidence of mutual TLS, not a failure -
            // provided the server's own certificate verified first.
            const unsigned long err = ERR_get_error();
            std::string reason = err != 0 ? ERR_reason_error_string(err) : "";
            if (reason.empty()) reason = "handshake ended";
            const bool cert_demand = reason.find("certificate") != std::string::npos ||
                                     reason.find("peer did not return") != std::string::npos ||
                                     reason.find("alert") != std::string::npos;
            result.mutual_tls_demanded = msg_trace.certificate_request_seen ||
                                         (result.server_cert_verified && cert_demand);
            if (!result.server_cert_verified) {
                result.detail = verify_state.first_error.empty() ? reason : verify_state.first_error;
            } else {
                result.detail = "server identity verified; handshake ended at the client-certificate demand (" + reason + ")";
            }
        } else {
            // TLS 1.3 completes the client handshake BEFORE the server validates the (empty) client
            // certificate, so the demand surfaces either in the message trace (a) or as a
            // post-handshake alert (c). Read once, bounded by the socket timeout, to see which.
            std::string alert_reason;
            unsigned char byte = 0;
            const int read_rc = SSL_read(ssl, &byte, 1);
            if (read_rc <= 0) {
                const int ssl_error = SSL_get_error(ssl, read_rc);
                if (ssl_error == SSL_ERROR_SSL || ssl_error == SSL_ERROR_SYSCALL) {
                    const unsigned long err = ERR_get_error();
                    alert_reason = err != 0 ? ERR_reason_error_string(err) : "";
                }
            }
            const bool alert_demand = alert_reason.find("certificate") != std::string::npos ||
                                      alert_reason.find("peer did not return") != std::string::npos;
            result.mutual_tls_demanded = msg_trace.certificate_request_seen || alert_demand;
            if (result.mutual_tls_demanded) {
                result.detail = "server identity verified; the server demanded a client certificate";
                if (!alert_reason.empty()) result.detail += " (" + alert_reason + ")";
            } else {
                result.detail = "handshake completed; the server did NOT request a client certificate";
            }
        }

        SSL_shutdown(ssl);
        SSL_free(ssl);
    } while (false);

    SSL_CTX_free(ctx);
    ::close(fd);
    ERR_clear_error();

    result.ok = result.server_cert_verified;
    return result;
}

} // namespace homeguardian::net
