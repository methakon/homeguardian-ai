# syntax=docker/dockerfile:1
#
# HomeGuardian AI hub — multi-stage container build (milestone D-01).
#
# Stage `build`   : compiles the C++20 hub binaries (no Node.js, no Python).
# Stage `test`    : builds and runs the existing C++ suite INSIDE the container,
#                   so "the tests pass" is proven against the container toolchain
#                   (`docker build --target test .`), and then runs the D-03 TLS
#                   end-to-end acceptance, which needs the OpenSSL toolchain and
#                   therefore runs HERE rather than on a host without it.
# Stage `runtime` : debian-slim + the hub binary only; non-root, no shell deps
#                   beyond bash (used by the entrypoint and health probe).
#
# The hub's all-in-one HTTP demo server binds LOOPBACK ONLY (127.0.0.1) by design;
# this image deliberately does not publish it. See deploy/docker/README.md.

# ---------------------------------------------------------------------------
# Base: debian:trixie-slim. The project's CMakeLists requires CMake >= 3.28.3
# (cmake_minimum_required(VERSION 3.28.3)); Debian bookworm ships 3.25.1 and
# fails at configure. Trixie ships CMake 3.31.6 / g++ 14 / glibc 2.41.
#
# The SAME base is used for the runtime stage on purpose: building against a
# newer glibc than the runtime image provides produces a binary that cannot start
# ("GLIBC_2.3x not found"), so the builder and the runtime must match.
FROM debian:trixie-slim AS build

# Dependencies are fetched by the project's own CMake FetchContent rules (pinned
# tags: spdlog v1.13.0, nlohmann/json v3.11.2), so the build needs network access
# once. Git + ca-certificates are required for those clones.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates libssl-dev openssl python3 \
 && rm -rf /var/lib/apt/lists/*
# libssl-dev/openssl  : D-03 requires OpenSSL for the ingestion boundary (TLS 1.2+)
#                       and the acceptance harness provisions its private CA with
#                       the openssl CLI. No hand-rolled cryptography.
# python3             : used ONLY by the D-03 acceptance harness (registry edits, a
#                       free-port probe, SQLite counts). It is a build/test-stage
#                       dependency and is deliberately NOT in the runtime image.

WORKDIR /src
# Whole source tree (see /.dockerignore for what is excluded: build dirs, .git,
# the untracked example/, stray databases and logs).
COPY . .

# Preflight: the vendored third-party sources must be IN THE BUILD CONTEXT. If a
# .dockerignore rule or an incomplete context drops them, CMake fails deep inside
# its generate step with "Cannot find source file", which reads like a CMake bug.
# This turns that into one clear line that names the cause.
RUN for required in third_party/sqlite/sqlite3.c third_party/catch2/src/catch_with_main.cpp \
                    src/net/DeviceProtocol.cpp CMakeLists.txt; do \
      test -s "$required" || { \
        echo "BUILD CONTEXT IS INCOMPLETE: $required is missing or empty." >&2; \
        echo "Check .dockerignore and that the build runs from the repository root." >&2; \
        exit 1; \
      }; \
    done \
 && echo "build context complete"

ARG HG_NPROC=4
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --target hg_alexa_bridge homeguardian hg_device_ingress hg_device_client \
           hg_ingress_probe \
      -- -j"${HG_NPROC}" \
 && strip build/hg_alexa_bridge build/homeguardian \
           build/hg_device_ingress build/hg_device_client build/hg_ingress_probe

# ---------------------------------------------------------------------------
FROM build AS test
# Proves the existing suite passes with the container's toolchain. ctest sets
# HG_SOURCE_DIR/WORKING_DIRECTORY itself, so the shipped-config test resolves.
RUN cmake --build build --target homeguardian_tests -- -j"${HG_NPROC}" \
 && ctest --test-dir build --output-on-failure \
 && HG_INGRESS_PATH=/src/build/hg_device_ingress \
    HG_CLIENT_PATH=/src/build/hg_device_client \
    bash scripts/verify_device_transport.sh \
 && HG_INGRESS_PATH=/src/build/hg_device_ingress \
    HG_CLIENT_PATH=/src/build/hg_device_client \
    bash scripts/verify_tls_identity.sh \
 && bash scripts/verify_ca_isolation.sh \
 && HG_INGRESS_PATH=/src/build/hg_device_ingress \
    HG_PROBE_PATH=/src/build/hg_ingress_probe \
    bash scripts/verify_ingress_healthcheck.sh

# ---------------------------------------------------------------------------
# Matches the builder's glibc (see the note above).
FROM debian:trixie-slim AS runtime

# bash: entrypoint + /dev/tcp health probe (no curl, no http client needed).
# bash: entrypoints + /dev/tcp health probe (no curl needed).
# libssl3t64 (trixie) / libssl3 (bookworm): the runtime side of the OpenSSL the
# ingestion boundary links. Installed explicitly so the dependency is visible in
# the image rather than resolved by luck. Nothing else is added: certificate
# provisioning happens on the HOST (tools/provision_ca.sh), so the runtime image
# carries no interpreter and no cert CLI.
RUN apt-get update \
 && apt-get install -y --no-install-recommends bash \
 && (apt-get install -y --no-install-recommends libssl3t64 \
     || apt-get install -y --no-install-recommends libssl3) \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 10001 hg \
 && useradd --uid 10001 --gid 10001 --no-create-home --home-dir /data \
            --shell /usr/sbin/nologin hg \
 && mkdir -p /data /data/ingress /etc/homeguardian \
 && chown -R 10001:10001 /data

COPY --from=build /src/build/hg_alexa_bridge   /usr/local/bin/hg_alexa_bridge
COPY --from=build /src/build/homeguardian     /usr/local/bin/homeguardian
COPY --from=build /src/build/hg_device_ingress /usr/local/bin/hg_device_ingress
COPY --from=build /src/build/hg_device_client  /usr/local/bin/hg_device_client
COPY --from=build /src/build/hg_ingress_probe  /usr/local/bin/hg_ingress_probe
COPY deploy/docker/entrypoint.sh         /usr/local/bin/hg-entrypoint
COPY deploy/docker/ingress-entrypoint.sh /usr/local/bin/hg-ingress-entrypoint
COPY deploy/docker/healthcheck.sh        /usr/local/bin/hg-healthcheck
COPY deploy/docker/ingress-healthcheck.sh /usr/local/bin/hg-ingress-healthcheck
COPY deploy/docker/config/hub.json       /etc/homeguardian/hub.json
COPY deploy/docker/config/ingress.json.example /etc/homeguardian/ingress.json.example
RUN chmod 0755 /usr/local/bin/hg-entrypoint /usr/local/bin/hg-ingress-entrypoint \
               /usr/local/bin/hg-healthcheck /usr/local/bin/hg-ingress-healthcheck \
 && chmod 0644 /etc/homeguardian/hub.json /etc/homeguardian/ingress.json.example \
 && chmod 0755 /usr/local/bin/hg_alexa_bridge /usr/local/bin/homeguardian \
               /usr/local/bin/hg_device_ingress /usr/local/bin/hg_device_client \
               /usr/local/bin/hg_ingress_probe

# Configuration paths (override at run time; the config and credentials are
# mounted read-only — never baked into a layer).
#
# NO TOKEN IS BAKED IN. The hub's demo token is a literal the demo config also carries, but
# baking it into the image hands every consumer of the image a credential-shaped value and
# makes the build emit a secrets-in-ENV warning that would hide a REAL one later. The demo
# deployment declares it in docker-compose.yml; the entrypoint falls back to it, loudly, for
# a bare local `docker run`. Nothing suppresses the warning instead.
ENV HG_PORT=8080 \
    HG_DATA_DIR=/data \
    HG_CONFIG=/etc/homeguardian/hub.json \
    HG_CREDS=/run/secrets/hg_creds \
    HG_INGRESS_CONFIG=/etc/homeguardian/ingress.json \
    HG_INGRESS_DATA=/data/ingress

# The whole point of D-01's persistence story: /data is the only writable path.
# Declaring the volume here means a fresh named volume inherits hg:hg ownership.
# /data/ingress is created (and owned) HERE on purpose: a named volume mounted at
# a path that does not exist in the image is created by Docker as root:root, so the
# non-root ingress could not write to it - it exited with "data directory not
# writable" until the directory existed in the image with the service's ownership.
VOLUME ["/data"]

USER 10001:10001
WORKDIR /data

# Informational only. 8080 is the hub's LOOPBACK-ONLY demo/dashboard listener and
# is published by the opt-in loopback override, never by the default deployment.
# 8443 is the D-03 device ingestion boundary: mutually authenticated TLS, speaking
# one protocol, serving no dashboard and no commands. It is the only port the
# default deployment publishes.
EXPOSE 8080 8443

# HUB health probe (HTTP /api/health on $HG_PORT). The `ingress` service OVERRIDES this in
# docker-compose.yml with its own probe: it runs a different binary on a different port, so this
# one can only ever fail there (it reported "unhealthy" while the ingress was serving TLS).
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["/usr/local/bin/hg-healthcheck"]

# exec form + `exec` in the script => the hub is PID 1 and receives SIGTERM.
ENTRYPOINT ["/usr/local/bin/hg-entrypoint"]
