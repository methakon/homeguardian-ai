# G05 / G08 — Handoff to Antigravity

**Audited revision:** `experiment/mi-a3-edge-service` @ `c8a8a78d4025e6795bad8d136d56bd2c0f9fa052`
**Audit branch (immutable evidence):** `experiment/mi-a3-edge-service-audit`
**Date:** 2026-10-20 · No handset state was modified.

---

## 1. Immutable artifact evidence — use these, do not re-derive

```
APK        homeguardian-edge-service.apk
size       1,651,248 bytes
sha256     cee0024c97f064045ff2d7829cf8e3f0f98e5944fe0095757044b98a37b3f172   (recomputed from the real file)
content    de0bdb22261dcdb49cb9c8609c2249eb26a9e6c45c978e12ff73f8416f1529aa   (5 entries; signature artefacts excluded)
package    org.homeguardian.edgeservice · 1.0.3 (versionCode 4) · minSdk 30 · targetSdk 30 · arm64-v8a only
signing    v1 verified; v2/v3 block present; v4 .idsig present
cert       CN=HomeGuardian Harness Debug (self-signed, DEBUG KEY) · serial 44E3EA415474EB06
           SHA-256 86:9B:0A:27:0F:D8:55:A2:CD:9E:B6:6C:E3:F8:04:3D:0E:DF:F9:F4:BF:2E:B6:55:2A:C3:F4:04:47:D0:28:1B
```

Documents in this audit branch:

| File | Contents |
|---|---|
| `docs/audit/g05_g08_artifact_record.json` | machine-readable manifest (source, artifact, package, signing, components, ABI, provenance) |
| `docs/audit/G05_G08_ARTIFACT_RECORD.md` | the same, human-readable, with evidence commands |
| `docs/audit/G05_G08_DEFECT_AND_BUILD_INTEGRITY.md` | why `e3116ac1…` could not install; the `supervisor.sh`/`SHA256SUMS` mismatch and its cause |
| `docs/audit/G05_G08_ROUTING_CONTROLS.md` | VPN routing vs private-endpoint validation, socket binding, DNS, reconnect, fail-closed |
| `docs/audit/G05_G08_RELEASE_ADVISORY.md` | installed-vs-candidate comparison, signing continuity, rollback limits |

## 2. Android feature / status section — truthful and concise

**Status:** candidate APK built from a clean tree at `c8a8a78`, verified by package inspection.

- Native **C++ MCP server** ships in `lib/arm64-v8a/libhg_edge_service.so` (unstripped, with
  `McpHttpServer`, `ToolRegistry`, `make_homeguardian_tools`, JSON-RPC, `tools/call`, protocol
  revision `2025-11-25`).
- **RFC6455 WebSocket JSON bridge** is implemented in the **Java** layer
  (`OutboundMcpBridge.java`), not in the native library — the split is intentional and coherent.
- **Operator authorization:** 256-bit CSPRNG device token (issued once on explicit operator action,
  revocable), production MCP bearer token never transmitted, ≤8 in-flight requests, `4403` revocation
  suppresses reconnect, consent-gated delivery in the native layer.
- **Routing:** route decisions come from platform `LinkProperties`, not subnet containment; the
  outbound socket is **bound to the VPN network** so a routing change between check and connect cannot
  leak the credential; DNS is re-resolved on every check; absent platform state and revoked devices
  both fail **closed**.
- **Install blocker fixed:** `resources.arsc` is stored uncompressed, satisfying the Android 11
  (`targetSdk ≥ 30`) install requirement. Verified in the shipped bytes.

## 3. What must NOT be claimed

- **No real-device installation** of this candidate was performed. Package inspection ≠ device acceptance.
- **No on-device run** of this candidate: no service start, MCP handshake, coordinator bridge, WebRTC or
  consent flow observed on hardware.
- **v2/v3/v4 signature validity is unverified** (`apksigner` needs a JVM; none on this workstation).
  Only v1 was verified; the rest is presence.
- **It is a debug-signed candidate**, not a release-signed production artifact.
- **VPN routing remains untested on hardware** (no consent granted, no peer enrolled).
- Byte-for-byte rebuild reproducibility was **not** re-verified in this audit (the content-digest tool
  was run successfully; a rebuild was not performed).

## 4. Actions requested of Antigravity

1. **Do not hand-edit `SHA256SUMS`.** Regenerate `deploy/linux_arm64/SHA256SUMS` from the committed bytes
   at the tip (`566c7f65…` for `supervisor.sh`) and add a CI check that recomputes every entry, so a
   manifest can never again describe bytes that were not committed. (`hg_mcp_server` and
   `homeguardian.demo.json` already verify.)
2. **Fix the stale comment** in `RouteSecurityPolicy` claiming `ACCESS_NETWORK_STATE` is absent from the
   manifest — the shipped APK declares it and it is operator-approved.
3. **Record build provenance in the build output**: have `build_service_apk.sh` write the source SHA,
   toolchain versions and the content digest beside the APK. Today the artifact carries no link to its
   commit.
4. **Decide the exported receiver**: `BootReceiver` is `exported=true` with no `android:permission`
   guard (both actions are system-protected broadcasts, so this is a judgement call, not an alarm).
5. **Treat the update as one-way**: signing is continuous so data is preserved on update, but Android
   refuses versionCode decreases — rollback means uninstall (destroys app data and the device token) or
   the stock-image path. Do not present uninstall as harmless rollback.

## 5. Boundaries observed

No install, replace, uninstall, reboot, reset, bootloader unlock, flash, erase, VPN consent or peer
enrolment. Device interaction was read-only: `adb devices`, `dumpsys package`, `pm path`, and one
read-only pull of the already-installed `base.apk` to compare signing identity. Recovery package and
native-Linux artifacts left untouched; no new Kaggle compute; PR #20 unmodified.
