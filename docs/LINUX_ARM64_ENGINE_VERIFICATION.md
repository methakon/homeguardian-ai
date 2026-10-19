# Linux ARM64 (glibc) MCP engine — verification record

Artifact built natively in an arm64 Ubuntu 24.04 container with g++ 13.3.0 (not a cross-compile),
then exercised in the same environment. This is **emulated ARM64 verification**, NOT physical Mi A3
acceptance.

```
size ....... 2,702,376 bytes
sha256 ..... 82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
type ....... ELF 64-bit LSB pie, ARM aarch64, GNU/Linux, interpreter /lib/ld-linux-aarch64.so.1
NEEDED ..... libstdc++.so.6  libm.so.6  libgcc_s.so.1  libc.so.6
```

Built twice; both builds produced the identical sha256, so the glibc ARM64 build is
byte-reproducible.

## Why a glibc build was required

The earlier Android artifact links against Bionic (`liblog.so`, interpreter
`/system/bin/linker64`) and therefore cannot run under Ubuntu, Debian or Alpine PRoot. `ldd` on this
artifact resolves every dependency against arm64 glibc:

```
libstdc++.so.6 => /lib/aarch64-linux-gnu/libstdc++.so.6
libm.so.6      => /lib/aarch64-linux-gnu/libm.so.6
libgcc_s.so.1  => /lib/aarch64-linux-gnu/libgcc_s.so.1
libc.so.6      => /lib/aarch64-linux-gnu/libc.so.6
/lib/ld-linux-aarch64.so.1
```

## Results — 16 passed, 0 failed

| # | Verification | Result |
|---|---|---|
| 1 | ARM64 ELF architecture and dependency compatibility | PASS — all deps resolve against arm64 glibc |
| 2 | Service startup | PASS — binds 127.0.0.1 only; RSS 19,640 kB under emulation |
| 3 | MCP initialize | PASS — serverInfo `homeguardian-mcp`, protocolVersion `2025-11-25`, session id issued |
| 4 | tools/list exposes exactly two read-only tools | PASS — `get_homeguardian_status`, `get_alert_summary` |
| 5 | tools/call get_homeguardian_status | PASS — real engine state, `read_only: true` |
| 6 | tools/call get_alert_summary | PASS — real engine state |
| 7 | Unauthorized refusal | PASS — 401 with no header, 401 with a bogus bearer |
| 8 | Session isolation | PASS — 400 with no session id, 404 with a bogus one |
| 9 | No public unauthenticated listener | PASS — only `127.0.0.1:8790` |
| 10 | Safe process shutdown | PASS — clean SIGTERM; `requests=8 errors=0` |

Sample response:

```
{"id":3,"jsonrpc":"2.0","result":{"content":[{"text":"{\"capture_enabled\":false,
 \"read_only\":true,\"state\":\"Running\"}","type":"text"}]}}
```

## Scope limit

This verifies the artifact and the protocol surface in an arm64 container. It does **not** verify
operation on the physical Mi A3, which still requires Termux and PRoot Ubuntu to be installed on the
device (blocked: Termux is not installed, and the USB link remains unstable).
