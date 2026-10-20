# G05 / G08 — Mi A3 Android edge service: artifact record and provenance

**Audited revision:** `experiment/mi-a3-edge-service` @ `c8a8a78d4025e6795bad8d136d56bd2c0f9fa052`
**Date:** 2026-10-20 · **No handset state was modified** (read-only device interaction only).

Machine-readable form: [`g05_g08_artifact_record.json`](g05_g08_artifact_record.json)

---

## 1. Verdict summary

| Item | Result |
|---|---|
| Corrected APK digest reproduces from the actual file | **PASS** |
| `resources.arsc` stored uncompressed (the API 30 install requirement) | **PASS** |
| Package / version / SDK levels read from the artifact | **PASS** |
| Signing schemes and certificate fingerprinted | **PASS** (v1 verified; v2/v3/v4 presence only — see 5.2) |
| C++ MCP architecture present in the packaged native library | **PASS** |
| RFC6455 JSON bridge present | **PASS** (Java layer, not native — see 6.2) |
| Operator authorization controls present | **PASS** |
| Source-to-binary mapping | **PARTIAL / BLOCKED** — the build script pins no source revision (see 2.3) |
| Real-device installation | **NOT PERFORMED** — a prohibited protected-hardware operation in this mission |
| Byte-for-byte rebuild reproducibility | **NOT VERIFIED** — rebuild not performed |

---

## 2. Source identity

### 2.1 The fix is the branch tip

```
experiment/mi-a3-edge-service      c8a8a78d4025e6795bad8d136d56bd2c0f9fa052
  subject: fix(android): store resources.arsc uncompressed so the APK can install on API 30
experiment/mi-a3-linux-runtime     26448d3b588ec4f4aeaff931ca27624e3ceba2b8  (contains it)
```

### 2.2 Build tree state

The APK was produced in worktree `hermes-mi-a3-edge-service`, which is at **`26448d3b…`**, branch
`experiment/mi-a3-linux-runtime`, **clean (0 dirty entries)** and contains `c8a8a78`. So the artifact
was built from a clean tree that includes the fix.

### 2.3 Provenance gap (BLOCKED)

`apk_service/build_service_apk.sh` **does not pin or embed a source revision**, and no digest is
recorded beside the APK in the build directory. The artifact therefore cannot be tied to a commit
by evidence *inside* the build itself — the mapping above is established by tree state and timestamp
correlation, not by a recorded link. Recommended: have the build write the source SHA, toolchain
versions and the content digest next to the APK (the repo already has
`scripts/apk_content_digest.py` for the digest half).

---

## 3. The artifact

```
filename : homeguardian-edge-service.apk
size     : 1,651,248 bytes
sha256   : cee0024c97f064045ff2d7829cf8e3f0f98e5944fe0095757044b98a37b3f172   <-- verified on the real bytes
content  : de0bdb22261dcdb49cb9c8609c2249eb26a9e6c45c978e12ff73f8416f1529aa
           (5 entries / 4,690,720 content bytes; signature artefacts excluded by the project's own tool)
companion: homeguardian-edge-service.apk.idsig (21,974 B) — APK Signature Scheme v4
```

The reported digest `cee0024c…b3f172` was **recomputed from the file**, not accepted:

```
$ sha256sum homeguardian-edge-service.apk
cee0024c97f064045ff2d7829cf8e3f0f98e5944fe0095757044b98a37b3f172  homeguardian-edge-service.apk
```

Eight ZIP entries, all normalised to `1980-01-01 00:00`:

```
AndroidManifest.xml · resources.arsc · classes.dex
lib/arm64-v8a/libhg_edge_service.so · lib/arm64-v8a/libc++_shared.so
META-INF/HARNESSD.SF · META-INF/HARNESSD.RSA · META-INF/MANIFEST.MF
```

---

## 4. Package identity

```
identifier      org.homeguardian.edgeservice
versionName     1.0.3        versionCode  4
minSdk          30           targetSdk    30        compileSdk 34
label           HomeGuardian Edge
permissions     INTERNET · FOREGROUND_SERVICE · RECEIVE_BOOT_COMPLETED · ACCESS_NETWORK_STATE
native code     arm64-v8a only
```

**Exported component inventory:**

| Component | Type | exported | Notes |
|---|---|---|---|
| `…edgeservice.MainActivity` | activity | **true** | launchable activity — expected |
| `…edgeservice.EdgeService` | service | **false** | correct for a foreground service |
| `…edgeservice.BootReceiver` | receiver | **true** | `BOOT_COMPLETED` + `MY_PACKAGE_REPLACED`; **no `android:permission` guard** |

The receiver is the only exported surface worth a second look. Both actions are system-protected
broadcasts, so third-party spoofing is not straightforward — but an exported receiver with no
permission is the pattern that turns into a finding later; worth an explicit decision.

No `<provider>` is declared.

---

## 5. Signing

### 5.1 What is present

**v1** (JAR: `HARNESSD.SF`, `HARNESSD.RSA`, `MANIFEST.MF`) · **v2/v3** (APK Signing Block magic
present) · **v4** (companion `.idsig`).

### 5.2 What was actually verified

I verified the **v1 certificate**: parsed from `META-INF/HARNESSD.RSA` and fingerprinted.

```
subject  C=US, O=HomeGuardian, CN=HomeGuardian Harness Debug   (self-signed)
serial   44E3EA415474EB06
validity 2026-10-10T18:44:21Z .. 2054-02-25T18:44:21Z
algo     sha256WithRSAEncryption
SHA-256  86:9B:0A:27:0F:D8:55:A2:CD:9E:B6:6C:E3:F8:04:3D:0E:DF:F9:F4:BF:2E:B6:55:2A:C3:F4:04:47:D0:28:1B
SHA-1    D5:31:23:97:A7:6B:AC:57:56:0B:C6:E9:49:F2:09:74:3A:D7:87:9C
```

**v2/v3/v4 cryptographic validity is NOT verified** — `apksigner` needs a JVM and no `java` exists
on this workstation. Only *presence* is established. This is stated rather than glossed because a
present signing block is not a valid signature.

### 5.3 Key classification (must not be glossed)

The certificate **says "Harness Debug" in its own subject** and is self-signed. It is a **debug
key**, not a production or release key. The build is correctly described as a *candidate built from
a clean tree*, **not** as release-signed, and no statement about it should imply a production key.

---

## 6. Architecture preservation (steps 5 of the mission)

### 6.1 Native C++ MCP server — present

`lib/arm64-v8a/libhg_edge_service.so` is an **unstripped ELF 64-bit aarch64 shared object** and
carries the MCP implementation as real symbols, not just strings:

```
homeguardian::mcp::McpHttpServer::start / serve / stats / set_allowed_origins
homeguardian::mcp::ToolRegistry, McpHttpConfig
homeguardian::mcp::make_homeguardian_tools(IAlexaCredentialVerifier&, IAlexaStatusProvider&)
```



### 6.2 RFC6455 JSON bridge — present, in the Java layer

Worth stating precisely, because a naive check gets this wrong: grepping the native library for
`Sec-WebSocket` / `HTTP/1.1 101` / `Upgrade` returns **zero** hits. The bridge is **not** in the
`.so`. It lives in `classes.dex` (`Sec-WebSocket`, `Upgrade`, `ws://`, `jsonrpc`) and in
`apk_service/src/org/homeguardian/edgeservice/OutboundMcpBridge.java` — the only tree file citing
RFC6455. The split is coherent: native C++ serves MCP over HTTP; Java implements the outbound
WebSocket JSON bridge to the coordinator.

### 6.3 Operator authorization controls — present

- 256-bit CSPRNG device auth token, revealed **once** on explicit operator action, revocable.
- The production MCP bearer token is read in-process and **never transmitted**.
- At most **8 in-flight** requests; excess is refused rather than queued without bound.
- A **4403** response sets `revoked` and **suppresses reconnect** (fail-closed after revocation).
- Native side: `ConsentGuardedDevice::authorize_delivery` gated on consent state.

---

## 7. Evidence commands

```bash
sha256sum homeguardian-edge-service.apk
$HOME/Android/Sdk/build-tools/34.0.0/aapt2 dump badging homeguardian-edge-service.apk
$HOME/Android/Sdk/build-tools/34.0.0/aapt2 dump xmltree --file AndroidManifest.xml homeguardian-edge-service.apk
python3 scripts/apk_content_digest.py homeguardian-edge-service.apk
unzip -l homeguardian-edge-service.apk
openssl pkcs7 -inform DER -in META-INF/HARNESSD.RSA -print_certs \
  | openssl x509 -noout -fingerprint -sha256 -subject -serial
strings -a lib/arm64-v8a/libhg_edge_service.so | grep -i 'mcp\|jsonrpc\|tools/call'
```
