# Google sign-in deployment and acceptance

Task: HG-WEB-PORTAL-GOOGLE-LOGIN-51 (tracked in issue #25 comments).

## Published deployment

https://homeguardian.berhampore.in/ now serves the cookieless OIDC gateway on the
existing approved HTTPS tunnel. The previous dashboard is retained in
`src/web/index.html`, served through the gateway, and hidden behind authentication
and household authorization. No second dashboard or public MCP port was added.

The dedicated owner-created Web client belongs to project
`homeguardian-ai-web-portal`. Its registered origin is
`https://homeguardian.berhampore.in`; its registered callback is
`https://homeguardian.berhampore.in/auth/callback`. The existing Tantra client and
its redirects were not modified. Do not create a duplicate client.

Client ID and secret are installed only in
`/opt/homeguardian/gateway/gateway.env` (0600). The secret is used for server-side
code exchange, not browser code. No credential JSON or environment file is tracked.

Gateway environment contract:

- HG_GOOGLE_CLIENT_ID and HG_GOOGLE_CLIENT_SECRET: dedicated client credentials.
- HG_PUBLIC_ORIGIN=https://homeguardian.berhampore.in
- HG_GOOGLE_REDIRECT_URI=https://homeguardian.berhampore.in/auth/callback
- HG_GATEWAY_BIND=127.0.0.1; HG_GATEWAY_PORT=8810
- HG_MCP_URL: existing loopback native read-only MCP endpoint.
- HG_MCP_TOKEN: server-only backend credential.
- HG_PORTAL_PATH: deployed copy of the existing src/web/index.html.

The tunnel's HomeGuardian route now targets http://127.0.0.1:8810 instead of the
old bridge on 8091. Other tunnel routes, mail, WireGuard and Alexa are unchanged.

## Login and refresh behavior

1. Open the published page and click Sign in with Google. Allow its popup.
2. The original tab generates a memory-only browser verifier. A same-origin POST
   starts login and binds its challenge to server-held state, nonce and PKCE.
3. Google redirects the popup to the callback. The server exchanges the code and
   validates the ID-token signature, issuer, audience, azp, time claims and nonce.
4. A single-use handoff code returns via exact-origin popup messaging. If Google
   severs the popup opener, the original tab retrieves completion using a bounded,
   expiring server rendezvous and its memory-only verifier. Both paths share
   single-use consumption; only the initiating tab can obtain the session.
5. The browser holds its token only in memory; no authentication cookies,
   localStorage or sessionStorage. Reloading requires a new sign-in.
6. Logout revokes the server session and clears memory. Switch account revokes the
   current session before starting another Google account-selection flow.

After authentication the existing dashboard shell is visible, with a VIEWER banner
and logout/account-switch controls. Every identity remains VIEWER. Protected
subject grants still control access to private household resources.

The owner has separately approved an outdoor-camera judge/reviewer policy:
`HG_OUTDOOR_REVIEW_ACCESS=google_viewers` permits every successfully Google-signed-in
account to read the configured outdoor-camera snapshots/status and aggregate
status/alerts. It does not grant private device inventory, microphone feeds,
consent changes, camera activation, uploads, deletion or administration. Without
that exact opt-in, the original subject-grant default-deny policy remains active.

The same reviewer mode permits GET `/api/scenarios` and POST
`/api/run-scenario?scenario=<catalogue-id>`. Simulation controls are interactive,
not camera controls. The gateway requires an authenticated memory-only session,
exact allowed Origin and an empty JSON POST body. It runs the trusted executable
named by `HG_REVIEW_BRIDGE_BIN` in a fresh private synthetic fixture, not the live
camera process. Each fixture has independent credentials and an independent audit
store; there is no HTTP listener or physical acquisition. Runs are bounded to one
child at a time, 15 seconds and 1 MiB combined output. Results are labelled
synthetic/isolated, and temporary state is removed after the child exits.

A callback `invalid_state` means its login attempt expired, was cancelled, was
already consumed, or lost its in-memory server state; it is not household denial.
Start again from the sign-in button in the original portal tab rather than
refreshing or sharing a callback URL containing a one-use Google code.

## Verified evidence

Focused offline suites (synthetic RSA/JWKS and token endpoint fixtures):

- node web_mcp/test_oauth.js: 263 assertions passed, 0 failed.
- node web_mcp/test_outdoor_review.js: 64 authorization assertions passed.
- HG_REVIEW_INTEGRATION=1 node web_mcp/test_outdoor_review.js: the same policy checks with real native scenario execution passed.
- node --test web_mcp/test_simulation_review.js: 16 tests passed, including all ten native scenarios and bounded-process failure controls.
- node web_mcp/test_client.js: 26 passed, 0 failed.
- node web_mcp/test_devices_dashboard.js: 161 passed, 0 failed.

Coverage includes token validation and expiry, state/nonce/replay, browser-bound
handoff, default-deny household isolation, explicit authorized-subject test seam,
MCP unavailable responses, logout/expiry, and cookieless security headers.
The production executable supports protected-file subject grants and the explicit
owner-enabled outdoor-review policy above. Backend failures are unavailable,
never fabricated zeros.

Public HTTPS probes after tunnel readback:

- /: 200, parser-blocking /assets/portal-session.js bootstrap present.
- /health: 200, oidc_configured=true, cookies=false.
- /api/status, /api/alerts, /api/devices, /api/camera/frame: anonymous 401.
- Browser: visible Sign in with Google page; an actual click opens Google's
  account sign-in prompt without redirect_uri_mismatch.

The deployed service subsequently recorded successful real Google token validation
and VIEWER session creation, but the owner reported a stuck completion page: the
original implementation depended solely on popup opener messaging. The replacement
has a regression harness executing the served scripts with opener=null against the
HTTP gateway; the original tab becomes authenticated and the dashboard is visible.
Real-account UI/logout acceptance of that replacement still requires a fresh owner
login. The task is not DONE; private-data acceptance is also blocked on the separate
household grant model. Do not close the tracking issue on fixture results.
