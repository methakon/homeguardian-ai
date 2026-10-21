"""Canonical refusal/accept vocabulary - OBSERVED from the owning lanes, never invented.

Provenance (read-only; these files belong to other lanes and were not modified):
  src/net/DeviceProtocol.cpp              @ 02ab07c   (C++ / Hermes 3)
  apk/tests/ObservationTransportPolicyTest.java  @ cfb96ef   (Android / Hermes 2)

Both lanes agree on the device-level tokens, so this set is the wire contract rather than a
local convention. Anything the LAN/mTLS harness emits must be a member, or integration will
mis-classify a refusal it does not recognise.

Local extension: this harness suffixes a token with its subject ("device_revoked:eluga-i7-01").
The token BEFORE the first ':' must still be canonical, which test_vocabulary_conformance.py
enforces.
"""

CANONICAL = {
    "accepted",
    # device identity
    "device_revoked",
    "device_unknown",
    "device_unpaired",
    "identity_mismatch",
    # consent - required by HG-NET-READY-14 item 2, and PRESENT in both owning lanes
    "consent_missing",
    "consent_withdrawn",
    # envelope / schema (not this harness's layer, listed for completeness)
    "invalid_value",
    "unknown_kind",
    "unknown_observation_type",
    "unknown_source_type",
}

PROVENANCE = {
    "src/net/DeviceProtocol.cpp": "02ab07c",
    "apk/tests/ObservationTransportPolicyTest.java": "cfb96ef",
}

# the canonical tokens that a LAN-first camera-to-hub path must be able to refuse with
REQUIRED_FOR_LAN_PATH = {"device_revoked", "device_unknown", "device_unpaired",
                         "identity_mismatch", "consent_missing", "consent_withdrawn"}


def base(token: str) -> str:
    """The canonical part of a possibly suffixed token."""
    return token.split(":", 1)[0]
