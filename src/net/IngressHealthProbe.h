#pragma once

// Ingress endpoint probe - the service-specific health check for the `ingress` container.
//
// WHY THIS EXISTS: the runtime image carries a HEALTHCHECK that probes the HUB's loopback HTTP API
// on port 8080. The ingress container runs hg_device_ingress on 8443 only, so it inherited a probe
// that could never pass: `docker ps` reported it "unhealthy" while it was serving mutual TLS
// correctly. The fix must NOT be a plain TCP-connect check, which passes against ANY process
// holding the port - TLS or not, ours or not - and would hide exactly the class of fault a health
// check is for.
//
// WHAT THIS PROVES:
//   1. the endpoint accepts a TCP connection (byte < 1);
//   2. a TLS handshake completes far enough that the SERVER's certificate is presented;
//   3. that certificate chains to the configured private CA *and* covers the expected name or
//      address - both checks are done by OpenSSL's own verification, reusing the same calls the
//      device client uses (TlsTransport.cpp: min TLS 1.2, load_verify_locations, set1_host /
//      set1_ip_asc, SSL_VERIFY_PEER);
//   4. the server DEMANDS a client certificate (mutual TLS), reporting that as its own field.
//
// WHAT THIS DOES NOT PROVE - stated plainly so nobody has to infer it: it does NOT authenticate a
// device. A hub-side container deliberately holds no device private key (device keys are mounted
// into no hub container), so this probe presents no client certificate, sends no observation and
// changes no replay state; the handshake is expected to END at the server's client-certificate
// demand. Device authentication is exercised by the device-transport acceptance suite with a real
// device certificate. "Health" here means "the ingress is up and it is OURS".
//
// NO DISABLE SWITCH: there is no option to skip chain/name verification (D-03.1 forbids a
// verification-disable option), and the CA file must load or the probe fails.

#include <string>

namespace homeguardian::net {

struct IngressProbeOptions {
    std::string connect_host = "127.0.0.1";  // where to dial
    int port = 8443;                         // the ingress's only listener
    std::string ca_file;                     // REQUIRED: the private CA that must have signed the server cert
    std::string verify_name;                 // a DNS name that must appear in the certificate's SAN
    std::string verify_ip;                   // ...or an address that must appear as an IP SAN
    int timeout_ms = 4000;                   // connect + handshake budget
};

struct IngressProbeResult {
    bool ok = false;  // the server's identity was verified -> the ingress is up and it is ours
    bool connected = false;
    bool handshake_started = false;
    bool server_cert_verified = false;
    bool mutual_tls_demanded = false;
    std::string endpoint;         // "host:port" exactly as dialled
    std::string tls_version;      // e.g. "TLSv1.3"
    std::string server_subject;   // e.g. "CN=homeguardian-hub"
    std::string server_san;       // e.g. "DNS:ingress,DNS:localhost,IP:10.20.30.6"
    std::string detail;           // bounded explanation; never carries key material

    // One line for the container log / health-check output.
    std::string summary() const;
};

IngressProbeResult probe_ingress_endpoint(const IngressProbeOptions& options);

} // namespace homeguardian::net
