# Runtime dependency checklist — hg_mcp_server (Linux ARM64 / glibc)

## Required at load time

| Library | Source | Present on Ubuntu 24.04 ARM64? |
|---|---|---|
| libstdc++.so.6 | `apt install libstdc++6` | yes (base) |
| libm.so.6 | glibc | yes (base) |
| libgcc_s.so.1 | `apt install libgcc-s1` | yes (base) |
| libc.so.6 | glibc | yes (base) |
| ld-linux-aarch64.so.1 | `apt install libc6` | yes (base) |

Verify on the phone:

    ldd ./hg_mcp_server
    # every line must resolve; any "not found" is a blocker

    readelf -d ./hg_mcp_server | grep NEEDED

Measured on the build system: all five resolve against `/lib/aarch64-linux-gnu/`.

## NOT required

- No SQLite package: the engine vendors sqlite3 statically.
- No OpenSSL: the MCP server does not use TLS (loopback only).
- No Python, Node or Java runtime.
- No network dependencies at runtime.

## Not required but used for the build only

`cmake`, `g++`, `git`, `ca-certificates` — only if building ON the phone. The deployment path ships a
prebuilt binary precisely so the phone never has to compile.

## Build-time caveat (does not affect deployment)

`CMakeLists.txt` fetches `spdlog v1.13.0` and `nlohmann_json v3.11.3` from GitHub during configure, so
an on-device build needs network access and a writable source tree. Shipping the prebuilt binary
avoids this entirely.
