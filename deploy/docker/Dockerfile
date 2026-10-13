# syntax=docker/dockerfile:1
#
# HomeGuardian AI hub — multi-stage container build (milestone D-01).
#
# Stage `build`   : compiles the C++20 hub binaries (no Node.js, no Python).
# Stage `test`    : builds and runs the existing C++ suite INSIDE the container,
#                   so "the tests pass" is proven against the container toolchain
#                   (`docker build --target test .`).
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
      build-essential cmake git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Whole source tree (see /.dockerignore for what is excluded: build dirs, .git,
# the untracked example/, stray databases and logs).
COPY . .

ARG HG_NPROC=4
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --target hg_alexa_bridge homeguardian -- -j"${HG_NPROC}" \
 && strip build/hg_alexa_bridge build/homeguardian

# ---------------------------------------------------------------------------
FROM build AS test
# Proves the existing suite passes with the container's toolchain. ctest sets
# HG_SOURCE_DIR/WORKING_DIRECTORY itself, so the shipped-config test resolves.
RUN cmake --build build --target homeguardian_tests -- -j"${HG_NPROC}" \
 && ctest --test-dir build --output-on-failure

# ---------------------------------------------------------------------------
# Matches the builder's glibc (see the note above).
FROM debian:trixie-slim AS runtime

# bash: entrypoint + /dev/tcp health probe (no curl, no http client needed).
RUN apt-get update \
 && apt-get install -y --no-install-recommends bash \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 10001 hg \
 && useradd --uid 10001 --gid 10001 --no-create-home --home-dir /data \
            --shell /usr/sbin/nologin hg \
 && mkdir -p /data /etc/homeguardian \
 && chown -R 10001:10001 /data

COPY --from=build /src/build/hg_alexa_bridge /usr/local/bin/hg_alexa_bridge
COPY --from=build /src/build/homeguardian   /usr/local/bin/homeguardian
COPY deploy/docker/entrypoint.sh  /usr/local/bin/hg-entrypoint
COPY deploy/docker/healthcheck.sh /usr/local/bin/hg-healthcheck
COPY deploy/docker/config/hub.json /etc/homeguardian/hub.json
RUN chmod 0755 /usr/local/bin/hg-entrypoint /usr/local/bin/hg-healthcheck \
 && chmod 0644 /etc/homeguardian/hub.json \
 && chmod 0755 /usr/local/bin/hg_alexa_bridge /usr/local/bin/homeguardian

# Configuration paths (override at run time; the config and credentials are
# mounted read-only — never baked into a layer).
ENV HG_PORT=8080 \
    HG_DATA_DIR=/data \
    HG_CONFIG=/etc/homeguardian/hub.json \
    HG_CREDS=/run/secrets/hg_creds \
    HG_TOKEN=demo-token

# The whole point of D-01's persistence story: /data is the only writable path.
# Declaring the volume here means a fresh named volume inherits hg:hg ownership.
VOLUME ["/data"]

USER 10001:10001
WORKDIR /data

# Informational: the listener is loopback-only inside the container. Publishing
# this port would expose a server that was security-reviewed for loopback only.
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["/usr/local/bin/hg-healthcheck"]

# exec form + `exec` in the script => the hub is PID 1 and receives SIGTERM.
ENTRYPOINT ["/usr/local/bin/hg-entrypoint"]
