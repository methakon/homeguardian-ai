# Android camera relocation: verified gap and safe integration prerequisite

## Current findings

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

One of the following must be established before claiming safe automatic recovery:

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
