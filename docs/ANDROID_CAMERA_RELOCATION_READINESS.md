# Android camera relocation: verified gap and safe integration prerequisite

## Deployed worker verification — 2026-10-26

Tested source `73b09e61bfb1b39438990dffb43d089c10932814` was pushed and the remote
SHA read back before deployment. APK 1.1.3/code 11 was installed IN PLACE, with
no enrolled-service uninstall or data clearing. Installed APK SHA-256 matched the
verified candidate exactly; the existing signing identity and normal permissions
were preserved. The existing camera configuration and VPN were not edited.

Actual phone readback:

    before update: no_frame, observations=0, fetch_failures=654
    after update: analyzed, observations=4
    later sample: analyzed, observations=75, frames_fetched=76
    after Activity closed / screen off: observations=242, frames_fetched=243
    observations added across that interval: 167
    decode_failures=0, fetch_failures=1, current-run alerts_committed=0
    discovery_state=found, discovery_source=rediscovered
    discovery_scanned_hosts=254
    identity_gate=authorized_enrolled_mac_kernel_netlink

The screen-off interval included an explicit bounded wait of at least 20 seconds.
The camera was recovered from the already-stale attachment by the phone itself;
no operator rebind, workstation locator, credentials spray or synthetic frame was
used. The one verification frame was not substituted into analysis; later normal
fetches supplied fresh observations. These counters prove existing native
motion/statistical processing, not semantic person/fall coverage or pre-filter
integration. No new persisted alert was manufactured.

With the activity closed/screen off, both real VPS worker-origin MCP calls also
succeeded: Running/read_only/capture-enabled, and 13 durable alerts/highest warning.
The 13 are retained historical alerts, not a claim of 13 new post-update detections.
Missing and invalid bearer tests against the private reader each returned HTTP 401.

Acceptance: WORKER-VERIFIED stale-attachment recovery and fresh autonomous analysis.
NOT full production certification. Phone reboot, a newly forced DHCP lease change,
cross-subnet/NAT movement and workstation-shutdown continuity were NOT VERIFIED.
The existing /24 scope and network routing constraints still apply. No R2, Alexa,
pre-filter, router/firewall/mail service or other VM rollout was performed here.

## Approved implementation checkpoint — 2026-10-26

The owner explicitly approved the existing locator, Android JNI and APK manifest
edits plus tested APK deployment. The source now selects bounded netlink reads on
Android, keeps the Linux backend, rejects ambiguous MAC/IP passes, persists locator
cooldown/hints and invokes existing recovery from the single authorized camera
loop. Probe-worker authorization is serialized. After relocation, analysis receives
the next genuine fetch, not a cached-frame substitution. No pre-filter files changed.

Verification before installation:

- New parser/identity harness: 1,092 assertions, including malformed/incomplete/
  interrupted dumps, sender/sequence validation, bounds, NOARP tunnel records and
  conflicting device mappings. The final harness passed AddressSanitizer and
  UndefinedBehaviorSanitizer as well as the normal compiler gate.
- Existing locator suite: 1,856 assertions / 27 cases, all passed.
- Android recovery contract/API30 compilation: 3 tests passed; decoder: 3 tests
  passed; MCP session harness: 13 checks passed.
- The ACTUAL production reader was packaged in an ordinary-UID diagnostic APK and
  returned a completed table with two eligible records. Explicit netlink bind is
  denied on this phone; unbound sendto auto-assignment is required. Empty hardware
  addresses on legitimate NOARP tunnel entries must be ignored, not treated as
  malformed Ethernet records. The diagnostic was removed and absence verified.
- Managed-service APK version 1.1.3/code 11 built successfully, API30/arm64-v8a,
  unchanged four normal permissions, same installed signing certificate. APK v3
  signature verified and netlink implementation symbols confirmed linked.
- Candidate APK SHA-256:
  `ea09f7a04279657f80697bac6f8050de60931d260e20a8dfb6be181353767f25`;
  size 1,810,992 bytes. Source SHA is recorded externally, not embedded in the APK.

This is source/build verification, NOT installation or fresh-camera acceptance.
The installed version-9 baseline was freshly read as no_frame, zero observations,
zero current-run alerts, zero decode failures and 654 fetch failures. Runtime
readback follows installation. Preserve app data; never uninstall the enrolled
service to downgrade. A separately signed, payload-identical forward-version
recovery candidate is prepared privately, but not installed or runtime certified.
Recovery retains the existing same-site /24 scope; arbitrary cross-subnet/NAT
migration is not certified by this change.

## Recheck on the actual Android phone — 2026-10-26

The other workstream DID implement MAC-anchored recovery and an autonomous
90-second watcher in `camera_ingest/recovery.py` and wire it into the Python relay
(commits `3e66284`, `37f8a78`, `c9483db`). These commits are already in main.
A fresh focused run of `python3 -m unittest camera_ingest.test_recovery -q` passed
28 tests. This verifies those recovery rules, not Android app deployment.

The production Android call site remains different:
`apk_service/jni/hg_edge_service_jni.cpp:371` sets `scan_enabled=false`, and its
status names `owner_approved_fixed_host_mac_no_rediscovery`. The default native
locator reads `/proc/net/arp`; Python invokes `ip neigh show`. Neither the Python
watcher nor an Android-compatible discovery provider is wired into that service.

A temporary diagnostic APK was built with the existing SDK/NDK for min/target/API
30. Its only requested permission was normal `INTERNET`. It was installed only on
the independently identity-verified enrolled handset; its installed APK SHA-256
matched the built artifact before execution. The completed test ran native code
from an explicit receiver under the ordinary app UID/SELinux domain, NOT ADB shell:

    ordinary_uid=true domain=untrusted_app
    proc_errno=13 socket_errno=0
    arp_ioctl_result=-1 arp_ioctl_errno=13
    netlink_errno=0 neighbor_rows=7 complete_ipv4_mac_rows=5

Both `/proc/net/arp` opening and the read-only `SIOCGARP` ioctl returned EACCES.
HOWEVER, a follow-up ordinary-app-UID test successfully used NETLINK_ROUTE /
RTM_GETNEIGH, returned seven neighbor messages and validated five records with an
IPv4 destination and a six-byte link-layer address. No private values were logged.
The earlier blanket implication that Android has no usable kernel-neighbor backend
was too broad: this is a verified candidate backend on the enrolled handset.

The ioctl used the empty address and emitted no network packet. The netlink dump
queried only the kernel; it sent no packet to a camera, changed no routes and used
no camera credential, host, MAC or media. An initial Activity-based attempt timed
out behind the UI; the completed receiver-based tests run without dismissing the
lock screen.

Final tested diagnostic APK SHA-256:
`80b569cef9820ebd1a650ac25958f085d976a441247a169b92ef24f2d414ead4`.
The temporary package was uninstalled and absence was read back. No existing
HomeGuardian APK/source, camera settings, VPN, OS permissions or services were
modified by this diagnostic. Only the temporary diagnostic APK was installed;
no SDK, privileged access or persistent service was added.

Next implementation: add bounded netlink dump/parsing as the Android locator I/O
backend; preserve the existing enrolled-MAC matching, conflict rejection, consent,
cooldown, credential-destination checks and single acquisition loop. Wire that
backend into the Android call site and verify actual relocated-camera processing.
Netlink capability alone is NOT a completed camera-recovery acceptance test.
Do not replace the other workstream's algorithm or turn on its Linux I/O blindly.
The owner's shared-file permission gate still applies to Android service wiring.

## Last acquisition evidence

The VPS worker can read both genuine phone MCP tools. This does not establish
camera acquisition. Direct native status samples on the installed phone reported
`no_frame`, zero processed observations, zero current-run committed alerts and
348 fetch failures. Previously stored alerts remain readable.

The intended camera is reachable at a moved address: a current local neighbor
entry matched its enrolled MAC before any authenticated request. Two authorized,
bounded snapshot GETs returned valid JPEGs with different hashes. This is evidence
that the camera is available, not that the phone is processing it. The known
read-only CGI stub answered 200 but yielded no identity attributes in that check;
no metadata discovery capability is inferred from that response.

No camera credential, endpoint, MAC, vendor UID, image or private source address
is included in this public record. No camera configuration/write route was used.
No phone camera reconfiguration was performed during this investigation.

## Why switching on the existing locator is not a fix

- `CameraProvisioner` stores an owner-approved private host and MAC in the app's
  protected camera document. The JNI camera path binds that host and explicitly
  sets `scan_enabled=false`.
- `CameraLocator.h` relies on `/proc/net/arp`; the Python recovery relies on
  `ip neigh`. These are not a proven discovery backend for an ordinary Android 11
  application UID. An ADB-shell neighbor lookup is not equivalent to an app lookup.
- `V380Discovery` implements a different vendor discovery protocol. Its MAC/ID
  parser does not establish JUAN/EseeCloud compatibility.
- Existing camera credentials must not be sprayed across addresses, and an HTTP
  server banner or JPEG response cannot authorize a destination. An old address
  can be reassigned to another host.

## External evidence and limits

Android documents the restriction on applications accessing `/proc/net` on
Android 10 and later:
https://developer.android.com/about/versions/10/privacy/changes#proc-net-filesystem

JUAN publishes IPCamSuite search tooling. Its release history mentions device
UID/MAC handling and CGI-based updates; the displayed fields therefore do not
establish a credential-free Android discovery identity contract:
https://doc.juancloud.com/docs/download/download-1e2r4pfl6bkht

A device-observed SOUP Discover exchange over UDP 9014 is documented for a JUAN
VRCAM. The published request contains that author's device identifier, and the
sample response carries Esee ID and transport information but no MAC:
https://lucasteske.dev/2019/06/reverse-engineering-cheap-chinese-vrcam-protocol/

Do not copy that sample identifier into a request, treat the response shape as
universal, or declare support for this enrolled unit without validation. This is
an investigation lead, not an implemented or accepted Android resolver.

## Required safe mechanism

Follow the existing network-independent architecture in
`HG_NET_ARCH_01_NETWORK_INDEPENDENT_CAMERA_INTEGRATION.md`: logical site/device
identity and credentials remain authoritative; current addresses are attachments.
The connector must be inside a network segment that can reach the camera and must
dial outward to the coordination plane. A VPN to the phone does not discover a
dumb camera's LAN lease or create routes through unrelated NAT segments.

The primary implementation route is now the verified ordinary-app-UID netlink
neighbor backend above, integrated with the existing enrolled-MAC policy. It still
needs bounded production parsing, Android wiring and real-camera acceptance.
If that backend proves insufficient on another device/network, the earlier
fallback options remain:

1. A supported discovery/identity contract for the actual JUAN unit that can be
   used by an unprivileged Android application before routing camera credentials.
   Validate identifiers, conflicts, deadlines, revocation and credential destinations
   on the enrolled unit. A self-reported UID/MAC is correlation, not cryptographic
   attestation; any change to the accepted identity contract requires explicit review.
2. An owner-approved always-on site-local connector with a supported authoritative
   lease/neighbor/device-identity source. Authenticate its reports, expire locations,
   validate conflicts and reauthorize use. Its own physical address may change;
   logical identity and outbound registration must not depend on that address.

Do not introduce a workstation dependency, enable root/privileged services,
install an unapproved SDK/runtime, enable ONVIF/RTSP, or change router settings as
an implicit workaround. A temporary operator-verified rebind would restore a
location hint only; it is not implementation of the owner's autonomous-DHCP
requirement and must not be presented as completion.

## Acceptance still required

The same enrolled camera must move address and recover without an operator or
workstation, with fresh frame/observation counters advancing on the phone. Stale
address takeover, missing/ambiguous identity, consent withdrawal, network loss,
queue cancellation and restart must fail closed. Existing observation, alert,
MCP and real-versus-synthetic boundaries must remain intact. A different subnet
requires a reachable site-local connector, not an expanded blind sweep.
